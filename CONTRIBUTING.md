# Contributing

Thank you for helping improve this board repository.

## Workflow

1. Fork the repository and create a topic branch.
2. Keep changes focused on one bug fix, example, or documentation update.
3. Update the affected README or documentation when paths, setup steps, or behavior change.
4. Open a pull request with a concise summary and validation notes.

## Validation

Pull requests should rely on GitHub Actions for build validation. When hardware behavior changes, include the board revision, example path, and the manual checks performed on the board.

## Examples

ESP-IDF examples are maintained under `examples/esp-idf/`. Arduino sketches and bundled libraries are maintained under `examples/arduino/`.

Examples inside bundled libraries are kept as upstream library material and are not part of product CI by default.
