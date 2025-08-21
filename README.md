# ESP32 Sensor Log Server

This project provides a reliable logging solution for ESP32-C3 sensor data. It uses a Python Flask web server running on an Orange Pi Zero 3 to receive HTTP POST requests containing sensor data and then logs the information to a text file. A simple web interface is provided for real-time viewing of the logs.

## Features

* **HTTP Endpoint:** Receives JSON data from the ESP32 via an HTTP POST request.

* **File-Based Logging:** Appends dated and timestamped sensor data to a text file for persistent storage.

* **Real-time Web Interface:** A simple web page that automatically refreshes to display the latest log entries.

* **Log Management:** A "Clear Log" button on the web interface allows you to clear the log file.

* **Systemd Integration:** Configured to run as a systemd service, ensuring the server automatically starts on boot and restarts if it fails.

## Requirements

### Hardware

* Orange Pi Zero 3

* ESP32-C3

### Software

* Python 3

* Flask library

* A Python virtual environment (`venv`)

## Setup

### 1. Project Files

First, ensure you have the `log_receiver.py` script and its dependencies installed in your virtual environment.

Place the script in the following directory:
`/home/wan/log/log_receiver.py`

### 2. Python Virtual Environment

If you haven't already, create and activate a virtual environment in the same directory as your script and install Flask.

```bash
cd /home/wan/log
python3 -m venv venv
source venv/bin/activate
pip install Flask
```

### 3. Systemd Service

To ensure the server runs reliably in the background, set it up as a systemd service.

Create the service file:

```bash
sudo nano /etc/systemd/system/bilik_log.service
```

Add the following content to the file, ensuring the paths are correct for your setup.

```ini
[Unit]
Description=ESP32 Sensor Log Server
After=network.target

[Service]
User=wan
Group=wan
WorkingDirectory=/home/wan/log
ExecStart=/home/wan/log/venv/bin/python3 /home/wan/log/log_receiver.py
Restart=always

[Install]
WantedBy=multi-user.target
```

Save the file and exit the editor.

### 4. Enable and Start the Service

Reload the systemd daemon to recognize the new service, then enable and start it.

```bash
sudo systemctl daemon-reload
sudo systemctl enable bilik_log.service
sudo systemctl start bilik_log.service
```

You can check the status to confirm it's running:

```bash
sudo systemctl status bilik_log.service
```

## Usage

### ESP32 Configuration

Configure your ESP32-C3 to send JSON data to the server's endpoint using an HTTP POST request. The URL for your endpoint will be `http://<your_orangepi_ip>:5001/log`.

Example JSON payload:

```json
{
  "event_message": "DHT11 sensor reading",
  "temperature": 25.5,
  "humidity": 60,
  "light": 500,
  "fan_status": true,
  "night_led_brightness": 128,
  "main_led_brightness": 255
}
```

### Web Interface

To view the logs in real-time, simply open a web browser and navigate to the following URL:
`http://<your_orangepi_ip>:5001`

## License

This project is released under the [MIT License](https://opensource.org/licenses/MIT).
