# STM32MP1 gateway — API documentation {#mainpage}

C application running on the **STM32MP157F-DK2** (Cortex-A7, Linux), cross-compiled
with `arm-linux-gnueabihf-gcc`. It polls Modbus RTU/TCP slaves, publishes telemetry to
AWS IoT over MQTT, stores data offline when the network is down and applies OTA updates.

* Architecture, threads, config flow and the payload contract: `DOCS/ARCHITECTURE.md`
* Build, CI and every test: `DOCS/CI_CD_GUIDE.md`, `DOCS/TESTS_GUIDE.md`

Start with `main.c`, then the module headers in `inc/`.
