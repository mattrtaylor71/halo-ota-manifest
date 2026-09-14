# HALO production firmware

**6.4.158 is the frozen default for all future firmware work.** Its paired scheduled OTA passed and the unit's daily schedule is restored to 02:00 Pacific. Start in [Arduino/HALOMAIN_rev1p5_modular](Arduino/HALOMAIN_rev1p5_modular), using this checkout or a reviewed descendant of `halo-v6.4.158`.

- [Frozen release handoff, recovery, and retention](Arduino/HALOMAIN_rev1p5_modular/docs/FROZEN_RELEASE_158.md)
- [Build and publish the next version](Arduino/HALOMAIN_rev1p5_modular/docs/BUILD_AND_RELEASE.md)
- [OTA results, failure analysis, and repeatable test procedure](Arduino/HALOMAIN_rev1p5_modular/docs/OTA_158_VALIDATION.md)
- [Machine-readable current baseline and preserved historical evidence](Arduino/HALOMAIN_rev1p5_modular/RELEASE_BASELINE.json)

The local tag `halo-v6.4.158` preserves exact artifact source `b6d06da5997e2252a3472697f30dc8111ab90367`. The current checkout also includes subsequent acceptance and handoff documentation. Do not reset it to an older release just because an old report calls that release “current.” The `Arduino/HALOMAIN_rev1` folder is legacy reference material.

The acceptance is scoped to the tested OTA path and supports a monitored rollout. USB-free, power-interruption, factory-station, and full product regression on 158 have not been newly qualified. Documentation preservation does not expand that scope.
