# Repository Agent Notes

## Validation before commit claims

Do not declare a patch committed, ready to merge, or validated until you have
validated it yourself. Prefer runtime checks with TCP input/screenshot tooling
when the change affects rendering, input, timing, or visible game behavior.
Record both the before/after condition or the regression comparison used.
