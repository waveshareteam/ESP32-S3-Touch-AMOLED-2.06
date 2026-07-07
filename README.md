# Waveshare ESP32-S3-Touch-AMOLED-2.06 Product Engineering Sample Program

ESP32-S3-Touch-AMOLED-2.06 ESP32-S3 2.06inch AMOLED Touch Watch Development Board, 32-bit LX7 dual-core processor, 410x502 pixels, QSPI interface, onboard dual digital microphone array, ESP32 with display.

---

## Configuration

You can find detailed configuration information on the product wiki page.

---

## Repository Layout

- ESP-IDF examples: `examples/esp-idf/`
- Arduino sketches and bundled libraries: `examples/arduino/`
- Factory firmware: `FirmWare/`
- Release helpers: `releases/`

See `docs/repository-structure.md` for the current layout policy, `docs/ci.md` for GitHub Actions build coverage, and `docs/firmware.md` for firmware artifact handling.

---

## CI and Releases

The `Build Examples` workflow validates first-party ESP-IDF and Arduino examples, then packages successful source builds into flashable `firmware-*` artifacts. Factory binaries under `FirmWare/` remain checked-in recovery artifacts and are not rebuilt by CI.

---

## Contributing

We welcome contributions. Here is how you can help:

1. Fork the repository.
2. Create a new branch for your feature or bug fix.
3. Commit your changes with clear descriptions.
4. Submit a pull request for review.

---

## Issues and Support

If you encounter any issues:

- Check the [Issues](https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-2.06/issues) section.
- Create a new issue with detailed information.
- Refer to the documentation for troubleshooting tips.
- Contact the Waveshare team and provide the order number to obtain technical support.

---

## License

This repository is licensed under the Apache License 2.0. See the `LICENSE` file for details.

---

## Acknowledgments

- Waveshare for their hardware platforms and software support.
- The Espressif team for their continuous support.
- Open-source contributors who make these projects possible.

---

Thank you for using Waveshare Electronics products.