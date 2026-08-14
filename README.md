> [!NOTE]
> This repository is an enhanced fork of the original project by [davidmpye/V10_Dyson_BMS](https://github.com/davidmpye/V10_Dyson_BMS).

# Dyson V10 Custom BMS Firmware

A written-from-scratch, unofficial replacement firmware and reverse-engineering resource for the **Dyson V10 Battery Management System (BMS)** board.

This project allows **YOU** to take total control of your battery pack—rebuild it, repair cell fault lockouts, or use it as a standalone power source for custom projects. Best of all, **you can install the new firmware using just a Raspberry Pi**—no expensive SWD programmer required!

![Dyson V10 BMS Closeup](https://github.com/davidmpye/V10_Dyson_BMS/assets/2261985/9c3c997c-1c46-4f77-aa3a-e4a8f9b940f4)

---

## ⚡ Status & What Works

* **Charging & Discharging:** Fully handles pack charging and drives the Dyson V10 vacuum motor.
* **Vacuum Communication:** Implements bidirectional USART comms to keep the cleaner running seamlessly.
* **Status & Error LEDs:** Displays vacuum fault states (e.g., Blocked / Filter missing) and BMS protection codes using the native LEDs.
* **Coulomb Counter:** Tracks state of charge (SoC) using the BMS IC's integrated coulomb counter.
* **Standalone Power:** Works without a Dyson vacuum connected, so you can use the pack to power DIY projects or power tools.

> [!TIP]
> See the **[Releases Section](https://github.com/Pr0metheus2/V10_Dyson_BMS/releases)** for a complete changelog, download binaries, and full feature lists for each release.

> [!NOTE]
> **Cell Balancing Note:** Like stock firmware, active/passive cell balancing is not implemented due to hardware limitations of the board.

---

## 📚 Documentation & Wiki Guides

Everything you need to get started is documented in detail in our **[Project Wiki](https://github.com/Pr0metheus2/V10_Dyson_BMS/wiki)**:

* ⚡ **[How to Flash the Firmware](https://github.com/Pr0metheus2/V10_Dyson_BMS/wiki/Flashing)** — Step-by-step guides for Raspberry Pi, Atmel-ICE, and WCH-LinkE.
* 🔋 **[New Firmware Operation](https://github.com/Pr0metheus2/V10_Dyson_BMS/wiki/New-firmware-operation)** — Gestures for capacity checks, firmware version display, and LED codes.
* 🛠️ **[Hardware Info & Datasheets](https://github.com/Pr0metheus2/V10_Dyson_BMS/wiki/Hardware-info)** — Pinouts, component specs, and schematic details.

---

## Looking for Dyson V11 Support?

If you are looking for replacement firmware for Dyson **V11** series packs, check out [vladislav1983/V11_Dyson_BMS](https)!
