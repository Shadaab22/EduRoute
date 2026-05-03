# EduRoute

EduRoute is a real-time, embedded IoT system that automates student attendance and enhances safety during school transportation using biometric authentication and live tracking.

The system leverages an ESP32 microcontroller, fingerprint sensor (R307), and NEO-6M GPS module to securely verify students during boarding and deboarding events while capturing timestamped location data. Upon successful authentication, attendance details are instantly transmitted to parents and school authorities via a Telegram Bot API, eliminating delays and manual errors.

A key strength of the system is its robust fallback mechanism—even if GPS fails to acquire location, attendance notifications are still delivered, ensuring uninterrupted communication. The system also includes an LCD display and buzzer feedback for real-time user interaction and stores enrollment data using Google Sheets integration.

Key Features:
 -> Biometric fingerprint-based student authentication.
 
 -> Real-time GPS tracking with Google Maps integration.
 
 -> Instant Telegram notifications to parents.
 
 -> Fallback communication during GPS failure.
 
 -> Fully embedded system (no heavy cloud dependency).
 
 -> Cost-effective and scalable for real-world deployment.

Tech Stack:
ESP32 • Embedded C • Arduino IDE • GPS (NEO-6M) • Fingerprint Sensor • Telegram Bot API • Google Sheets
