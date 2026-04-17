export USB_DEVICE=/dev/ttyACM0
export USB_BAUD=115200
export HOST=127.0.0.1
export PORT=8000

source .venv/bin/activate
python3 qrng_mcp_server.py