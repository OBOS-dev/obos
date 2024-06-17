# Contribution Guide (If you care)
## Commits
Make sure to make your commits with a couple changes each.
Don't forget to describe said changes with a relatively-descriptive commit message.
## Pull Requests
Any pull requests are welcome, as long as no Rust, or other Esoteric languages are included in the code, and they include a feature that actually matters.
All commits should be made on a separate local branch, pushed onto the main repo. You should make a pull request describing your changes (unless they're self explanatory because of the branch name).<br>
## Notes
Please don't add backdoors to the kernel or other parts of the OS.<br>
Don't modify src/oboskrnl/int.h, unless absolutely neccessary (it will cause a rebuild for any other contributors).