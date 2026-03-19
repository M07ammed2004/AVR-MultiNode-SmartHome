# 🏠 Dual-AVR Distributed Smart Home System

A high-performance, decentralized Smart Home automation system built using two **ATmega32** microcontrollers communicating via a custom Serial Protocol.



## 🛠️ System Architecture

The project follows a **Master-Slave Architecture** to offload tasks and ensure real-time responsiveness:

### 1. Master Node (The Brain)
* **Authentication:** Secure login via Keypad with passwords stored in **EEPROM**.
* **Interfaces:** 16x2 LCD menu system for local control.
* **Connectivity:** Integrated **Bluetooth module** (UART) for remote smartphone control.
* **Decision Making:** Sends high-level commands to the Slave node.

### 2. Slave Node (The Muscle)
* **Elevator Control:** High-precision movement using a **Stepper Motor** in Half-Step mode.
* **Environmental Sensing:** Custom bit-banged implementation of the **DHT11 protocol**.
* **Security:** **Servo Motor** control for door locking via Hardware PWM.
* **Interrupt-Driven:** Uses USART RX Interrupts for zero-latency command execution.

## 📡 Communication Protocol
The nodes communicate using a 3-byte packet system:
`[Device_ID] [Component_ID] [Action_Value]`

## 🚀 Technical Highlights
* **Software UART:** Implemented to provide additional communication channels.
* **Memory Management:** Efficient use of EEPROM for non-volatile data storage.
* **Timers/PWM:** Advanced configuration of Timer1 for 50Hz Servo signals.
* **Concurrency:** Interrupt-based design to handle sensors and motors simultaneously.

---
*Developed as part of a Multidisciplinary Engineering Portfolio.*