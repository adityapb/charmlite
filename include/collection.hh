#ifndef __CMK_collection_HH__
#define __CMK_collection_HH__

#include "callback.hh"
#include "chare.hh"
#include "ep.hh"
#include "locmgr.hh"
#include "message.hh"

namespace cmk {
    class collection_base_
    {
    protected:
        collection_index_t id_;

    public:
        collection_base_(const collection_index_t& id)
          : id_(id)
        {
        }
        virtual ~collection_base_() = default;
        virtual void* lookup(const chare_index_t&) = 0;
        virtual void deliver(message_ptr<>&& msg, bool immediate) = 0;
        virtual void contribute(message_ptr<>&& msg) = 0;

        template <typename T>
        inline T* lookup(const chare_index_t& idx)
        {
            return static_cast<T*>(this->lookup(idx));
        }
    };

    template <typename T, template <class> class Mapper>
    class collection : public collection_base_
    {
    public:
        using index_type = index_for_t<T>;

    private:
        locmgr<Mapper<index_type>> locmgr_;

        std::unordered_map<chare_index_t, message_buffer_t> buffers_;
        std::unordered_map<chare_index_t, std::unique_ptr<T>> chares_;

    public:
        static_assert(
            std::is_base_of<chare_base_, T>::value, "expected a chare!");

        collection(const collection_index_t& id,
            const collection_options<index_type>& opts, const message* msg)
          : collection_base_(id)
        {
            // need valid message and options or neither
            CmiEnforceMsg(
                ((bool) opts == (bool) msg), "cannot seed collection");
            if (msg)
            {
                auto& end = opts.end();
                auto& step = opts.step();
                // deliver a copy of the message to all "seeds"
                for (auto seed = opts.start(); seed != end; seed += step)
                {
                    auto view = index_view<index_type>::encode(seed);
                    // NOTE ( I'm pretty sure this is no worse than what Charm )
                    //      ( does vis-a-vis CKARRAYMAP_POPULATE_INITIAL       )
                    // TODO ( that said, it should be elim'd for node/groups   )
                    if (this->locmgr_.pe_for(view) == CmiMyPe())
                    {
                        // message should be packed
                        auto clone = msg->clone();
                        clone->dst_.endpoint().chare = view;
                        this->deliver_now(std::move(clone));
                    }
                }
            }
        }

        virtual void* lookup(const chare_index_t& idx) override
        {
            auto find = this->chares_.find(idx);
            if (find == std::end(this->chares_))
            {
                return nullptr;
            }
            else
            {
                return (find->second).get();
            }
        }

        void flush_buffers(const chare_index_t& idx)
        {
            auto find = this->buffers_.find(idx);
            if (find == std::end(this->buffers_))
            {
                return;
            }
            else
            {
                auto& buffer = find->second;
                while (!buffer.empty())
                {
                    auto& msg = buffer.front();
                    if (this->try_deliver(msg))
                    {
                        // if successful, pop from the queue
                        buffer.pop_front();
                    }
                    else
                    {
                        // if delivery failed, stop attempting
                        // to deliver messages
                        break;
                    }
                }
            }
        }

        bool try_deliver(message_ptr<>& msg)
        {
            auto& ep = msg->dst_.endpoint();
            auto* rec = record_for(ep.entry);
            auto& idx = ep.chare;
            auto pe = this->locmgr_.pe_for(idx);
            // TODO ( temporary constraint, elements only created on home pe )
            if (rec->is_constructor_ && (pe == CmiMyPe()))
            {
                auto* ch = static_cast<T*>((record_for<T>()).allocate());
                // set properties of the newly created chare
                property_setter_<T>()(ch, this->id_, idx);
                // place the chare within our element list
                auto ins = chares_.emplace(idx, ch);
                CmiAssertMsg(ins.second, "insertion did not occur!");
                // call constructor on chare
                rec->invoke(ch, std::move(msg));
                // flush any messages we have for it
                flush_buffers(idx);
            }
            else
            {
                auto find = chares_.find(idx);
                // if the element isn't found locally
                if (find == std::end(this->chares_))
                {
                    // and it's our chare...
                    if (pe == CmiMyPe())
                    {
                        // it hasn't been created yet, so buffer
                        return false;
                    }
                    else
                    {
                        // otherwise route to the home pe
                        // XXX ( update bcast? prolly not. )
                        send_helper_(pe, std::move(msg));
                    }
                }
                else
                {
                    // otherwise, invoke the EP on the chare
                    handle_(rec, (find->second).get(), std::move(msg));
                }
            }

            return true;
        }

        inline void deliver_now(message_ptr<>&& msg)
        {
            auto& ep = msg->dst_.endpoint();
            if (ep.chare == chare_bcast_root_)
            {
                auto root = locmgr_.root();
                auto* obj = static_cast<chare_base_*>(this->lookup(root));
                if (obj == nullptr)
                {
                    // if the object is unavailable -- we have to reroute it
                    // ( this could be a loopback, then we try again later )
                    send_helper_(locmgr_.pe_for(root), std::move(msg));
                }
                else
                {
                    // otherwise, we increment the broadcast count and go!
                    ep.chare = root;
                    ep.bcast = obj->last_bcast_ + 1;
                    handle_(record_for(ep.entry), static_cast<T*>(obj),
                        std::move(msg));
                }
            }
            else if (!try_deliver(msg))
            {
                // buffer messages when delivery attempt fails
                this->buffer_(std::move(msg));
            }
        }

        inline void deliver_later(message_ptr<>&& msg)
        {
            auto& idx = msg->dst_.endpoint().chare;
            auto pe = this->locmgr_.pe_for(idx);
            send_helper_(pe, std::move(msg));
        }

        virtual void deliver(message_ptr<>&& msg, bool immediate) override
        {
            if (immediate)
            {
                this->deliver_now(std::move(msg));
            }
            else
            {
                this->deliver_later(std::move(msg));
            }
        }

        virtual void contribute(message_ptr<>&& msg) override
        {
            auto& ep = msg->dst_.endpoint();
            auto& idx = ep.chare;
            auto* obj = static_cast<chare_base_*>(this->lookup(idx));
            // stamp the message with a sequence number
            ep.bcast = ++(obj->last_redn_);
            this->handle_reduction_message_(obj, std::move(msg));
        }

    private:
        using reducer_iterator_t =
            typename chare_base_::reducer_map_t::iterator;

        void handle_reduction_message_(chare_base_* obj, message_ptr<>&& msg)
        {
            auto& ep = msg->dst_.endpoint();
            auto& redn = ep.bcast;
            auto search = this->get_reducer_(obj, redn);
            auto& reducer = search->second;
            reducer.received.emplace_back(std::move(msg));
            // when we've received all expected messages
            if (reducer.ready())
            {
                auto comb = combiner_for(ep.entry);
                auto& recvd = reducer.received;
                auto& lhs = recvd.front();
                for (auto it = std::begin(recvd) + 1; it != std::end(recvd);
                     it++)
                {
                    auto& rhs = *it;
                    auto cont = *(rhs->continuation());
                    // combine them by the given function
                    lhs = comb(std::move(lhs), std::move(rhs));
                    // reset the message's continuation
                    // (in case it was overriden)
                    lhs->has_continuation() = true;
                    new (lhs->continuation()) destination(cont);
                }
                // update result's destination (and clear
                // flags) so we can send it along
                auto& down = reducer.downstream;
                if (down.empty())
                {
                    new (&(lhs->dst_)) destination(*(lhs->continuation()));
                    lhs->has_combiner() = lhs->has_continuation() = false;
                    cmk::send(std::move(lhs));
                }
                else
                {
                    CmiAssert(down.size() == 1);
                    lhs->dst_.endpoint().chare = down.front();
                    this->deliver_later(std::move(lhs));
                }
                // erase the reducer (it's job is done)
                obj->reducers_.erase(search);
            }
        }

        void handle_broadcast_message_(
            const entry_record_* rec, chare_base_* obj, message_ptr<>&& msg)
        {
            auto* base = static_cast<chare_base_*>(obj);
            auto& idx = base->index_;
            auto& bcast = msg->dst_.endpoint().bcast;
            // broadcasts are processed in-order
            if (bcast == (base->last_bcast_ + 1))
            {
                base->last_bcast_++;
                auto children = this->locmgr_.upstream(idx);
                // ensure message is packed so we can safely clone it
                pack_message(msg);
                // send a copy of the message to all our children
                for (auto& child : children)
                {
                    auto clone = msg->clone();
                    clone->dst_.endpoint().chare = child;
                    this->deliver_later(std::move(clone));
                }
                // process the message locally
                rec->invoke(obj, std::move(msg));
                // try flushing the buffers since...
                this->flush_buffers(idx);
            }
            else
            {
                // we buffer out-of-order broadcasts
                this->buffer_(std::move(msg));
            }
        }

        // get a chare's reducer, creating one if it doesn't already exist
        reducer_iterator_t get_reducer_(chare_base_* obj, bcast_id_t redn)
        {
            auto& reducers = obj->reducers_;
            auto find = reducers.find(redn);
            if (find == std::end(reducers))
            {
                auto& idx = obj->index_;
                // construct using most up-to-date knowledge of spanning tree
                auto up = this->locmgr_.upstream(idx);
                auto down = this->locmgr_.downstream(idx);
                auto ins = reducers.emplace(std::piecewise_construct,
                    std::forward_as_tuple(idx),
                    std::forward_as_tuple(std::move(up), std::move(down)));
                find = ins.first;
            }
            return find;
        }

        void handle_(const entry_record_* rec, T* obj, message_ptr<>&& msg)
        {
            // if a message has a combiner...
            if (msg->has_combiner())
            {
                // then it's a reduction message
                this->handle_reduction_message_(obj, std::move(msg));
            }
            else if (msg->is_broadcast())
            {
                this->handle_broadcast_message_(rec, obj, std::move(msg));
            }
            else
            {
                rec->invoke(obj, std::move(msg));
            }
        }

        inline void buffer_(message_ptr<>&& msg)
        {
            auto& idx = msg->dst_.endpoint().chare;
            this->buffers_[idx].emplace_back(std::move(msg));
        }
    };

    template <typename T>
    struct collection_helper_;

    template <typename T, template <class> class Mapper>
    struct collection_helper_<collection<T, Mapper>>
    {
        static collection_kind_t kind_;
    };
}    // namespace cmk

#endif
