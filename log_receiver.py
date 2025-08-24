import datetime
import os
import logging
from flask import Flask, request, render_template_string

# Create a Flask application instance
app = Flask(__name__)

# Specify the path for the log file
log_file_path = os.path.join('/home/wan/log/', 'sensor_log.txt')

def format_malay_date(dt):
    """Formats a datetime object to a Malay date string."""
    # The format_malay_date function is no longer needed since the
    # ESP32 will provide a single log message with a timestamp.
    # However, to avoid a breaking change, we can leave this stub.
    return dt.strftime('%d/%m %H:%M:%S')

# HTML template for the web page with Tailwind CSS
# This template includes a script to automatically refresh the log display
HTML_TEMPLATE = """
<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>ESP32 Sensor Log</title>
    <script src="https://cdn.tailwindcss.com"></script>
    <style>
        body {
            font-family: 'Inter', sans-serif;
        }
        .modal-overlay {
            position: fixed;
            top: 0;
            left: 0;
            width: 100%;
            height: 100%;
            background-color: rgba(0, 0, 0, 0.5);
            display: none;
            justify-content: center;
            align-items: center;
            z-index: 1000;
        }
        .fade-in-out {
            animation: fadeEffect 3s forwards;
        }
        @keyframes fadeEffect {
            0% { opacity: 1; }
            80% { opacity: 1; }
            100% { opacity: 0; }
        }
    </style>
</head>
<body class="bg-gray-100 min-h-screen p-4 sm:p-8">
    <div class="max-w-4xl mx-auto bg-white shadow-xl rounded-lg p-6 sm:p-8">
        <h1 class="text-3xl sm:text-4xl font-bold text-gray-800 mb-4 text-center">ESP32 Sensor Log</h1>
        <p class="text-sm sm:text-base text-gray-600 text-center mb-6">
            Real-time log of sensor data and events from your ESP32-C3.
        </p>
        <div class="flex flex-col items-center mb-4">
            <button id="clear-logs-btn" class="bg-red-500 hover:bg-red-600 text-white font-bold py-2 px-6 rounded-full shadow-lg transition-colors duration-200">
                Clear Log
            </button>
            <div id="status-message" class="mt-2 text-sm text-green-600 font-semibold hidden"></div>
        </div>
        <div class="bg-gray-800 text-white rounded-lg p-4 sm:p-6 overflow-x-auto h-[60vh] max-h-[700px] overflow-y-scroll">
            <pre id="log-content" class="text-xs sm:text-sm leading-relaxed"></pre>
        </div>
    </div>
    
    <!-- Confirmation Modal -->
    <div id="confirmation-modal" class="hidden modal-overlay">
        <div class="bg-white rounded-lg p-6 shadow-xl w-11/12 max-w-sm text-center">
            <p class="text-lg font-semibold text-gray-800 mb-4">Are you sure you want to clear the logs?</p>
            <div class="flex justify-around">
                <button id="confirm-yes" class="bg-red-500 hover:bg-red-600 text-white font-bold py-2 px-6 rounded-full transition-colors duration-200">
                    Yes
                </button>
                <button id="confirm-no" class="bg-gray-300 hover:bg-gray-400 text-gray-800 font-bold py-2 px-6 rounded-full transition-colors duration-200">
                    No
                </button>
            </div>
        </div>
    </div>

    <script>
        const logElement = document.getElementById('log-content');
        const clearBtn = document.getElementById('clear-logs-btn');
        const modal = document.getElementById('confirmation-modal');
        const confirmYes = document.getElementById('confirm-yes');
        const confirmNo = document.getElementById('confirm-no');
        const statusMessage = document.getElementById('status-message');

        // Function to fetch logs from the server
        async function fetchLogs() {
            try {
                const response = await fetch('/logs_data');
                if (!response.ok) {
                    throw new Error('Failed to fetch logs');
                }
                const logData = await response.text();
                
                // Update the content and scroll to the bottom
                logElement.textContent = logData;
                logElement.scrollTop = logElement.scrollHeight;

            } catch (error) {
                logElement.textContent = `Error: ${error.message}`;
            }
        }

        // Fetch logs on page load
        fetchLogs();

        // Refresh logs every 3 seconds
        setInterval(fetchLogs, 3000);

        // Show the confirmation modal
        clearBtn.addEventListener('click', () => {
            modal.style.display = 'flex';
        });

        // Handle the "No" button click
        confirmNo.addEventListener('click', () => {
            modal.style.display = 'none';
        });

        // Handle the "Yes" button click
        confirmYes.addEventListener('click', async () => {
            try {
                const response = await fetch('/clear_logs');
                if (response.ok) {
                    // Update log display
                    fetchLogs();
                    // Show a success message
                    statusMessage.textContent = 'Log file cleared!';
                    statusMessage.classList.remove('hidden');
                    statusMessage.classList.add('fade-in-out');
                    setTimeout(() => {
                        statusMessage.classList.remove('fade-in-out');
                    }, 3000);
                } else {
                    throw new Error('Failed to clear logs on the server.');
                }
            } catch (error) {
                statusMessage.textContent = `Error: ${error.message}`;
                statusMessage.classList.remove('hidden');
            } finally {
                // Ensure the modal is hidden regardless of outcome
                modal.style.display = 'none';
            }
        });
    </script>
</body>
</html>
"""

# Define a function to write the incoming log data to the file
def write_log(data):
    try:
        # Ensure the directory exists before attempting to write the file
        os.makedirs(os.path.dirname(log_file_path), exist_ok=True)
        
        # Open the log file in append mode and write the new entry
        with open(log_file_path, 'a') as f:
            log_entry = data.get('event_message', 'No event message provided.')
            f.write(log_entry + '\n')
            
    except IOError as e:
        # Print an error message if the file cannot be written
        print(f"Error writing to log file: {e}")

# Define the route that will receive POST requests from the ESP32
@app.route('/log', methods=['POST'])
def receive_log():
    if request.is_json:
        log_data = request.get_json()
        write_log(log_data)
        return "Log received", 200
    else:
        return "Request must be JSON", 400

# New route to serve the main web page
@app.route('/', methods=['GET'])
def serve_webpage():
    return render_template_string(HTML_TEMPLATE)

# New route to serve the raw log data to the web page's JavaScript
@app.route('/logs_data', methods=['GET'])
def get_logs_data():
    try:
        with open(log_file_path, 'r') as f:
            return f.read(), 200, {'Content-Type': 'text/plain'}
    except FileNotFoundError:
        return "Log file not found.", 404, {'Content-Type': 'text/plain'}

# New route to clear the log file
@app.route('/clear_logs', methods=['GET'])
def clear_logs():
    try:
        if os.path.exists(log_file_path):
            os.remove(log_file_path)
            return "Log file cleared successfully", 200
        else:
            return "Log file not found", 404
    except Exception as e:
        return f"Error clearing log file: {e}", 500

# Run the Flask app
if __name__ == '__main__':
    logging.getLogger('werkzeug').setLevel(logging.ERROR)
    app.run(host='0.0.0.0', port=5001)

