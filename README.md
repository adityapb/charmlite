# CharmLite

Status:
- Requires that `${env:CHARM_HOME}` points to a valid Charm++ build.
    - The lightest possible build is:
    `./build AMPI-only <triplet> <compiler>? --with-production -DCSD_NO_IDLE_TRACING=1 -DCSD_NO_PERIODIC=1`
- Only tested with non-SMP builds, SMP builds ~~currently crash~~:
    - ~~This can be fixed by correctly isolating globals as Csv/Cpv.~~
    - Probably fixed but needs more testing.
- Minimal support for collection communication:
    - Broadcasts and reductions only work for groups.
        - Plan to use Hypercomm distributed tree creation scheme for chare-arrays:
            - [Google doc write-up.](https://docs.google.com/document/d/1hv-9qm1dXR8R1VJXgtyFHuhTUoa_izrm-jDXPqqkpas/edit?usp=sharing)
            - [Hypercomm implementation.](https://github.com/jszaday/hypercomm/blob/main/include/hypercomm/tree_builder/tree_builder.hpp)
- No support for node-groups yet.
    - Very easy to do: add `<bool NodeLevel>` to existing group constructs.
- Add support for chare-arrays with a fixed _or_ initial size.
- Add support for "location records" that indicate migratibility.
    - How should users specify whether elements can/not migrate?

Overall... need more examples; feel free to _try_ porting your favorite example. (Be aware of collection communications limitations.)
