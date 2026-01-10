\# DoorControl – System Overview (README)



\*\*Document Version:\*\* 1.0



This document uses stable, numbered sections to allow precise referencing by humans and AI agents.



\## 1. Purpose and Scope



`DoorControl` is a modular ESP32-based system for \*\*secure door / garage opening\*\* and \*\*remote actor control\*\*, designed to run on multiple hardware platforms (ESP32-C3, ESP32-S3, M5Stack variants, minimal remotes).



The system intentionally distinguishes between:



\* \*\*High-security door control\*\* (anti-replay, anti-copy, challenge–response)

\* \*\*Convenience-oriented actor control\*\* (relays, lights, fog lamps, etc.) with OTA capability



This README provides a \*\*system-level overview\*\* and explains how the individual programs relate to each other. It is written to be understandable by both \*\*humans\*\* and \*\*AI agents\*\* (e.g. Codex, Claude) to enable correct maintenance, extension, and development of new variants.



For protocol details and timing values, see:



\* `TECH\_OVERVIEW.md`

\* `USER\_GUIDE.md`



---



\## 2. High-Level Architecture



The system consists of three logical roles:



1\. \*\*Door Receiver\*\* – fixed installation at the door/garage (secure)

2\. \*\*Controller\*\* – handheld or in-vehicle remote (secure + optional actor control)

3\. \*\*Actor\*\* – relay-based actuator (less security, OTA-enabled)



Communication is based on \*\*ESP-NOW\*\* with a fixed WiFi channel. OTA updates (where supported) use standard WiFi + ArduinoOTA.



```

Controller  <==== secure ESP-NOW ====>  DoorRECEIVER

&nbsp;    |

&nbsp;    +==== ESP-NOW ====> Actor (relay, OTA)

```



---



\## 3. Security Model



\### 3.1 Door / Garage Control (High Security)



The door-opening path is explicitly designed to be:



\* non-recordable

\* non-replayable

\* resistant to copying and timing attacks



Key properties:



\* HMAC-SHA256 based \*\*challenge–response\*\*

\* Per-session nonces and session IDs

\* Time-to-live (TTL) enforcement

\* Allowlist of sender MACs and shared secrets

\* No rolling counters and \*\*no NVS usage\*\* for secrets



This security model is implemented jointly by:



\* `DoorRECEIVER`

\* Controllers with Door capability (e.g. `ControllerOTA\_DC\_GEN\_II`)



\### 3.2 Actor Control (Lower Security)



Actor control is optimized for convenience and OTA update capability:



\* ESP-NOW command/status messages

\* MAC-based pairing

\* Optional power-sense feedback

\* ArduinoOTA via WiFi



Actor control \*\*does not\*\* implement the same cryptographic challenge–response as the door path.



---



\## 4. Active and Maintained Programs



\### 4.1 Door Receiver



\*\*`DoorRECEIVER/`\*\*



\* Secure receiver installed at the door or garage

\* Controls the actual door relay

\* Validates all door-open requests using challenge–response

\* Visual feedback via WS2812 LED



This component is mandatory for all secure door installations.



---



\### 4.2 Controllers



\#### 4.2.1 `ControllerOTA\_DC\_GEN\_II`



The \*\*primary and most complete controller implementation\*\*.



Capabilities:



\* Secure door opening (compatible with `DoorRECEIVER`)

\* Actor control (relay on/off)

\* OTA updates via WiFi (long-press)

\* Multiple hardware platforms selected via `#define` in `doorLockData.h`



Supported platforms include:



\* M5Stack (Atom / display variants)

\* Original controller hardware

\* Switch\_Light variant

\* Minimal "Remote" variant (deep-sleep, one-shot operation)



This controller supersedes all earlier DC controller variants.



---



\### 4.3 Actors



\#### 4.3.1 `ActorOTA\_DC`



\* Standard DC-compatible actor

\* ESP-NOW controlled relay

\* Optional power-sense input

\* OTA update support



\#### 4.3.2 `ActorOTA\_DC\_LIGHT`



\* Hardware-light variant of `ActorOTA\_DC`

\* No power-sense input

\* Inverted relay logic



Both actors are designed to work with DC controllers.



---



\### 4.4 Standalone / Separate Lines (Not Deprecated)



\#### 4.4.1 `DoorSENDER/`



\* Minimal, secure, door-only sender

\* OLED + button

\* Implements the same challenge–response protocol as DC controllers

\* No actor control, no OTA



Useful for minimal or dedicated door remotes.



\#### 4.4.2 `RemoteSwitchOTA/ControllerOTA` and `ActorOTA`



\* Independent remote-switch system

\* ESP-NOW relay control + OTA

\* No door security protocol



This line is \*\*not deprecated\*\*, but separate from DoorControl-DC.



---



\## 5. Deprecated Components



The following directories are deprecated and superseded by `ControllerOTA\_DC\_GEN\_II`:



\* `RemoteSwitchOTA/ControllerOTA\_DC/`

\* `RemoteSwitchOTA/ControllerOTA\_DC\_M5/`

\* `RemoteSwitchOTA/ControllerOTA\_DC\_GEN/`



They remain in the repository for reference only.



---



\## 6. Configuration



All active programs rely on a \*\*user-provided configuration header\*\*:



\* `doorLockData.h` (not committed)

\* Template: `doorLockDataExample.h`



This file defines:



\* WiFi channel

\* MAC addresses (receiver, actor, controllers)

\* Shared secrets for door security

\* Platform selection defines



---



\## 7. Documentation Files



\* \*\*`README.md`\*\* (this file)



&nbsp; \* System overview and relationships

\* \*\*`TECH\_OVERVIEW.md`\*\*



&nbsp; \* Protocol structure, message formats, timing, security logic

\* \*\*`USER\_GUIDE.md`\*\*



&nbsp; \* Flashing, pairing, operation, and usage instructions



Together, these three files define the complete conceptual model of the DoorControl system.



---



\## 8. Protocol Compatibility Matrix



The following matrix defines which components are expected to interoperate directly.



| Component               | DoorRECEIVER       | ActorOTA\_DC     | ActorOTA\_DC\_LIGHT |

| ----------------------- | ------------------ | --------------- | ----------------- |

| DoorSENDER              | ✅ Secure door only | ❌               | ❌                 |

| ControllerOTA\_DC\_GEN\_II | ✅ Secure door      | ✅ Actor control | ✅ Actor control   |

| ControllerOTA           | ❌                  | ✅               | ❌                 |



Legend:



\* ✅ = intended and supported

\* ❌ = not supported / not part of design



This matrix is normative for new development.



---



\## 9. How to Extend the System (Guidance)



\### 9.1 Adding a New Controller Variant



When creating a new controller variant:



\* Base it on `ControllerOTA\_DC\_GEN\_II` whenever possible

\* Reuse the existing door protocol unchanged

\* Select hardware differences exclusively via `#define` in `doorLockData.h`

\* Do \*\*not\*\* introduce new persistent storage for security state

\* Keep door control and actor control logic separated in code paths



If the controller is door-only and minimal, consider following the `DoorSENDER` pattern instead.



\### 9.2 Adding a New Actor Variant



When adding a new actor:



\* Start from `ActorOTA\_DC` or `ActorOTA\_DC\_LIGHT`

\* Keep ESP-NOW message formats backward compatible

\* Avoid adding cryptographic assumptions to the actor layer

\* OTA must remain optional and isolated from ESP-NOW logic



\### 9.3 Modifying the Door Security Protocol



Changes to the door protocol are \*\*high-impact\*\* and must:



\* Preserve challenge–response semantics

\* Preserve anti-replay guarantees

\* Remain stateless across power cycles

\* Be reflected consistently in:



&nbsp; \* `DoorRECEIVER`

&nbsp; \* all door-capable controllers

&nbsp; \* `TECH\_OVERVIEW.md`



---



\## 10. Documentation Relationship



This repository intentionally uses three complementary documents:



\* \*\*`README.md`\*\* (this file)



&nbsp; \* System architecture

&nbsp; \* Component relationships

&nbsp; \* Security domains



\* \*\*`TECH\_OVERVIEW.md`\*\*



&nbsp; \* Message formats (byte-level)

&nbsp; \* Timing constraints (TTL, delays)

&nbsp; \* Cryptographic primitives and flows



\* \*\*`USER\_GUIDE.md`\*\*



&nbsp; \* Flashing instructions

&nbsp; \* Pairing and setup

&nbsp; \* Normal operation and troubleshooting



All three documents together define the full specification of the DoorControl system.



---



\## 11. Design Intent (Important for AI Agents)



When extending or modifying this system:



\* Do not weaken the door security model

\* Do not introduce persistent counters or NVS-based secrets

\* Treat door control and actor control as \*\*separate trust domains\*\*

\* Prefer extending `ControllerOTA\_DC\_GEN\_II` instead of creating new controllers

\* Maintain strict compatibility with the door protocol defined in `TECH\_OVERVIEW.md`



AI agents should treat these constraints as \*\*hard requirements\*\*, not suggestions.



