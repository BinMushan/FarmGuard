# Pin mapping (ESP32 DevKit C)

| Device | Wokwi ID | Pin | ESP32 GPIO |
|---|---|---|---|
| Soil moisture (pot) | pot_moist | SIG | 34 |
| Soil temperature (NTC) | ntc1 | OUT | 35 |
| Soil pH (pot) | pot_ph | SIG | 32 |
| Nitrogen (pot) | pot_n | SIG | 33 |
| Phosphorus (pot) | pot_p | SIG | 36 (VP) |
| Potassium (pot) | pot_k | SIG | 39 (VN) |
| Air temp / humidity | dht1 | SDA | 14 |
| OLED SSD1306 | oled1 | SDA / SCL | 21 / 22 |
| Button irrigation | btn_irr | 1.l | 16 |
| Button fertilizer | btn_fert | 1.l | 4 |
| Button pesticide | btn_pest | 1.l | 17 |
| LED green (Necessary) | led_g | via r_g | 25 |
| LED yellow (Risky) | led_y | via r_y | 26 |
| LED red (Unnecessary) | led_r | via r_r | 27 |
| Buzzer | buz1 | pin 2 | 18 |

Power: 3V3 to all sensors and OLED. GND.1 sensors, GND.2 OLED and LEDs, GND.3 buttons and buzzer.
All analog inputs are on ADC1 because ADC2 does not work while Wi-Fi is active.
