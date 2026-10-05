# IoT-Based Intelligent Farm Decision & Waste Detection System

An ESP32 field node that does more than monitor: it checks whether a farmer's
**planned action** (irrigate, fertilize, spray) is actually needed, classifies it as
**Necessary / Risky / Potentially Unnecessary**, and estimates the avoidable waste.

> Status: field node simulated in Wokwi. Cloud backend, LLM and WhatsApp alerts are planned (see `cloud/`).

## Architecture

![Architecture](docs/architecture.drawio)

Open `docs/architecture.drawio` at https://app.diagrams.net (File > Open from > Device).

```
Sensors -> ESP32 -> Wi-Fi/HTTP -> Cloud backend (weather API + crop/cost DB)
        -> Decision engine -> Waste/cost estimate -> LLM explanation
        -> WhatsApp / Dashboard -> Farmer
```

## Repository layout

```
iot-farm-decision-system/
├── README.md
├── .gitignore
├── wokwi/                  Complete Wokwi project (field node)
│   ├── sketch.ino          ESP32 firmware
│   ├── diagram.json        Circuit
│   └── libraries.txt       Arduino libraries
├── docs/
│   ├── architecture.drawio System architecture diagram
│   └── pin-mapping.md      Pin and wiring reference
└── cloud/
    └── README.md           Planned backend (not implemented yet)
```

## Run the simulation

1. Create a new **ESP32** project at https://wokwi.com.
2. Copy `wokwi/sketch.ino`, `wokwi/diagram.json` and `wokwi/libraries.txt` into the matching tabs.
3. Press **Play**.

Serial monitor commands (115200 baud): `rain <mm>`, `crop <1-3>`, `act <0-3>`, `help`.

## Hardware (simulated)

| Real sensor | Wokwi substitute |
|---|---|
| Capacitive soil moisture | Potentiometer |
| Analog pH probe | Potentiometer |
| RS485 NPK probe (N, P, K) | 3 potentiometers |
| DS18B20 soil temperature | NTC temperature sensor |
| DHT22 | DHT22 |

## Roadmap

- [x] Field node firmware + Wokwi circuit
- [ ] Cloud API (decision engine, database)
- [ ] Weather API integration
- [ ] LLM explanation module
- [ ] WhatsApp alerts and web dashboard
- [ ] Real hardware build

## License

Add a license (e.g. MIT) before publishing.
