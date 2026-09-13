# Initial source provenance

The initial public source snapshot was exported from the last committed private integration baseline, commit `cd66f2b` (`Merge expanded visibility toggle fix`).

It was exported into a new repository instead of publishing the private Git history. That history contains large generated builds, frame captures, executable-analysis artifacts, local machine paths, and failed experiments that are neither required to build the mod nor appropriate for a public source repository.

The export preserved the committed mod implementation in `ThirdParty/3Dmigoto`, made one build-portability correction to an unrelated upstream project path, and added only:

- the minimum OpenVR and OpenXR build dependencies with their licenses
- the DirectXTK libraries expected by the existing Visual Studio project and their MIT notice
- the six active Metro shader replacement sources
- focused tests and clean public documentation
- the installer template and reproducible package-manifest tooling, without a binary payload

The uncommitted single-pass performance experiment present in the private working directory at export time was not included.
