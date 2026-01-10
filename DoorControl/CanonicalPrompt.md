Canonical Prompt – DoorControl

Context
You are working on the DoorControl system.
This repository is a security-critical embedded system based on ESP32 and ESP-NOW.

The authoritative documentation consists of exactly three files:

README.md (system architecture and rules)

TECH_OVERVIEW.md (protocols, message formats, crypto, timing)

USER_GUIDE.md (operation and flashing)

The README uses stable numbered sections.
All references to architecture and rules MUST follow those sections exactly.

1. System Understanding (mandatory)

Before making any changes or generating code:

Read and understand:

README.md §1–§11

TECH_OVERVIEW.md completely

Treat all statements in:

README §3 (Security Model)

README §8 (Protocol Compatibility Matrix)

README §11 (Design Intent)
as hard requirements, not suggestions.

If any requirement is unclear, ask for clarification before generating code.

2. Security Constraints (non-negotiable)

You MUST respect the following constraints:

Door / garage control security is defined in README §3.1

Actor control is a separate trust domain (README §3.2)

Never weaken or bypass the door challenge–response protocol

Do NOT introduce:

persistent counters

NVS-stored secrets

rolling codes

Door security must remain stateless across power cycles

Door and Actor logic must remain separated in code paths

The safety extension (DoorRECEIVER_SAFETY / DoorSLAVE_SAFETY):

is OPTIONAL and must work without slave present

is a SAFETY layer, NOT a security layer

uses NO cryptography for permits (separate from security protocol)

must NOT weaken the existing door security model

Violating any of these rules is considered an incorrect solution.

3. Allowed Base Implementations

When creating or modifying components, use ONLY these bases:

Door receiver:
DoorRECEIVER/ (standard)
DoorRECEIVER_SAFETY/ (with optional safety permit protocol, README §4.1)

Primary controller base:
ControllerOTA_DC_GEN_II (README §4.2.1)

Actor base:
ActorOTA_DC or ActorOTA_DC_LIGHT (README §4.3)

Safety slave (optional):
DoorSLAVE_SAFETY/ (README §4.1a)

Other variants are deprecated or separate lines and must not be extended unless explicitly requested.

4. Compatibility Rules

Follow the Protocol Compatibility Matrix (README §8) strictly.

Do not introduce new communication paths

Do not allow unsupported component pairings

Backward compatibility with existing DoorRECEIVER protocol is mandatory

5. Extension Rules

If you are asked to extend the system:

For controllers: follow README §9.1

For actors: follow README §9.2

For protocol changes: follow README §9.3

For safety extensions: the safety permit protocol is optional; future door receiver variants must preserve both main door security compatibility and optional safety permit behavior

All changes to protocols MUST be reflected consistently in:

code

TECH_OVERVIEW.md

6. Code Generation Rules

When generating code:

Target Arduino IDE

.ino file must reside in a directory with the same name

Use existing message structs and ESP-NOW initialization patterns

Do not invent new configuration mechanisms

Assume doorLockData.h exists and follows doorLockDataExample.h

Generated code must be:

complete

compilable

consistent with the existing architecture

7. Output Expectations

Your output must include:

A short explanation referencing README sections
(e.g. “This follows README §3.1 and §9.1”)

The full code or diff

Any required documentation changes (explicitly listed)

Do not simplify, merge, or redesign the system unless explicitly instructed.