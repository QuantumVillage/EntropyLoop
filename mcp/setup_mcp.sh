mkdir /opt/qrng-mcp/
cp ./ /opt/qrng-mcp/
cd /opt/qrng-mcp/

python3 -m venv .venv
source .venv/bin/activate
pip install -r requirements.txt