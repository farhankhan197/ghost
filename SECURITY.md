# Security

Ghost handles repository policy, Git hooks, and attribution metadata. Please report security issues privately before public disclosure.

## Reporting

Email: farhankhan.code@gmail.com

Include:

- affected Ghost version or commit
- operating system
- reproduction steps
- expected and actual behavior
- impact on policy enforcement, attribution integrity, hooks, or Git notes

## Security-Sensitive Areas

- policy loading and `--config-ref`
- Git note creation, verification, and signatures
- hook installation and execution
- path normalization across platforms
- attribution carry-forward after rewrites, merges, and checkouts
- CI workflows that decide whether a PR can merge

Do not open a public issue for a vulnerability until it has been triaged.

