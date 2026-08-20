# Contributing to LidarScan

Thanks for the interest — bug reports, suggestions, feature requests, and
pull requests are all welcome.

## Reporting a bug

- Use the [bug report issue template](.github/ISSUE_TEMPLATE/bug_report.md).
- Include your device model and app version (both are on the Profile tab).
- Say which sensor you were using — COIN-D6, Mid-360, STL-27L, or none.
- Attach the app's Send-logs bundle if you can: **Profile → Send logs**. It
  captures what the crash recorder and processing pipeline actually wrote,
  which is far more useful than a description alone.

## Proposing a feature

- Use the [feature request issue template](.github/ISSUE_TEMPLATE/feature_request.md).
- Describe the problem you're hitting, not just the feature you want — it's
  easier to evaluate a request when the underlying problem is clear.
- Explain why it matters: who runs into this, how often, how much it hurts.

## Code contributions

- Branch off `main` and open a pull request against `main`.
- Keep the test suites green before opening the PR:
  - `android/`: `./gradlew test` and the connected tests
  - `engine/`: `ctest`
- User-facing strings are covered by wording-law tests — instructions are
  capped at six words. Check the existing `*WordingTest.kt` files under
  `android/core/src/test/kotlin/com/lidarscan/core/` for the pattern before
  adding new UI copy.
- Match the style of the surrounding code. Don't introduce a new convention
  in a file that doesn't already have one.

## House rule: no claims without measured evidence

Every number in this repo's README and in the app itself came from an
actual measurement — a soak test, a checksum count, a logged frame rate —
not an estimate or a marketing figure. If your PR adds a claim (a
performance number, an accuracy figure, a "works on X" statement), it needs
to come from a real, reproducible measurement, and the PR description
should say how you got it. This is the one house rule that matters most
here.
