import sys
import random
import time
from PyQt6.QtWidgets import (QApplication, QMainWindow, QVBoxLayout, QHBoxLayout, 
                             QWidget, QLabel, QTextEdit, QPushButton, QSpinBox)
from PyQt6.QtCore import pyqtSignal, QThread, Qt
from PyQt6.QtGui import QPainter, QColor, QBrush, QPen

# Configuration
SERIAL_PORT = '/dev/tty.usbmodem1101' # Adjust to your actual serial port (e.g., COM3 on Windows)
BAUD_RATE = 115200

logeaux = "0000000000000000000000000000000000000099999900000099000000990000000099000000990000990000009900000000990000009900009900000099000000009900000099000099000000990000000099009900990000009900990000000000009999990000000000990000000000000000000099000000000000000000"

class SerialWorker(QThread):
    """
    Handles reading from the serial port in a separate thread to keep the UI responsive.
    Falls back to a mock generator if the serial port is unavailable.
    """
    data_received = pyqtSignal(str)

    def run(self):
        try:
            import serial
            with serial.Serial(SERIAL_PORT, BAUD_RATE, timeout=1) as ser:
                print(f"Connected to {SERIAL_PORT}")
                while not self.isInterruptionRequested():
                    try:
                        line = ser.readline().decode('utf-8', errors='ignore').strip()
                        if line.startswith("H_min:") and "Data:" in line:
                            hex1 = ser.readline().decode('utf-8', errors='ignore').strip()
                            hex2 = ser.readline().decode('utf-8', errors='ignore').strip()
                            self.data_received.emit(f"{line} {hex1}{hex2}")
                    except Exception as e:
                        print(f"Read error: {e}")
                        break
        except (ImportError, OSError) as e:
            print(f"Serial connection failed ({e}). Starting mock data generator for demonstration.")
            self.mock_run()

    def mock_run(self):
        """Generates fake data in the specified format for testing."""
        base_hex = "c4e441871b5f1dd50dd7915b89c1733fcc62f548f453545ee48d59d63a9dbedc8f75685116a81a72cb02fec716770278118765126f63281ff2a5c9aab755ced927db67e613d96552e8febee2cf19bddd2ec2cccb543055ed3159287d30de38aa7fb01bae71ba02c326502010ead442263c18aecb1fa2c87aa1c1d5883d1c3b6e"
        h_min = 7.6781
        r_val = 1554
        
        while not self.isInterruptionRequested():
            # Randomize the hex string slightly to show animation
            hex_list = list(base_hex)
            for _ in range(10):
                idx = random.randint(0, len(hex_list)-1)
                hex_list[idx] = hex(random.randint(0, 15))[2:]
            current_hex = "".join(hex_list)
            
            # Vary H_min slightly
            current_h = h_min + random.uniform(-0.05, 0.05)
            
            data_str = f"H_min: {current_h:.4f} | R: {r_val} | Data: {current_hex}"
            self.data_received.emit(data_str)
            time.sleep(0.1) # Update rate (10Hz)

class VisualizerWidget(QWidget):
    """
    Custom widget to draw the geometric interpretation of the random data.
    """
    def __init__(self):
        super().__init__()
        self.bytes_data = b''
        self.setMinimumHeight(300)
        # Dark background for better contrast with colors
        self.setStyleSheet("background-color: #2b2b2b; border-radius: 8px;")

    def update_data(self, hex_data):
        try:
            self.bytes_data = bytes.fromhex(hex_data)
            self.update() # Trigger a repaint
        except ValueError:
            pass

    def paintEvent(self, event):
        if not self.bytes_data:
            return

        painter = QPainter(self)
        painter.setRenderHint(QPainter.RenderHint.Antialiasing)

        w = self.width()
        h = self.height()
        
        # Calculate grid dimensions
        # 128 bytes fits nicely into a 16x8 grid
        cols = 16
        rows = (len(self.bytes_data) + cols - 1) // cols
        
        # Margins and cell sizing
        margin = 20
        available_w = w - 2 * margin
        available_h = h - 2 * margin
        
        cell_w = available_w / cols
        cell_h = available_h / rows
        
        # Size of the shape within the cell
        shape_size = min(cell_w, cell_h) * 0.85
        
        # Centering offsets
        offset_x = margin + (available_w - (cols * cell_w)) / 2 + (cell_w - shape_size) / 2
        offset_y = margin + (available_h - (rows * cell_h)) / 2 + (cell_h - shape_size) / 2

        for i, byte in enumerate(self.bytes_data):
            row = i // cols
            col = i % cols
            
            x = margin + col * cell_w + (cell_w - shape_size) / 2
            y = margin + row * cell_h + (cell_h - shape_size) / 2
            
            # Map byte value (0-255) to a color using HSV
            # Hue: The byte value itself (mapped to 0-359 degrees)
            # Saturation: High for vivid colors
            # Value: High for brightness
            hue = int((byte / 255.0) * 360)
            color = QColor()
            color.setHsv(hue, 200, 255)
            
            painter.setBrush(QBrush(color))
            painter.setPen(Qt.PenStyle.NoPen)
            
            # Draw a circle for each byte
            painter.drawEllipse(int(x), int(y), int(shape_size), int(shape_size))

class MainWindow(QMainWindow):
    def __init__(self):
        super().__init__()
        self.setWindowTitle("Entropy Source Visualizer")
        self.resize(900, 700)
        
        # Main layout container
        central = QWidget()
        self.setCentralWidget(central)
        layout = QVBoxLayout(central)
        layout.setSpacing(15)
        
        # --- Header Stats Section ---
        stats_layout = QHBoxLayout()
        self.lbl_hmin = QLabel("H_min: --")
        self.lbl_r = QLabel("R: --")
        
        # Styling for stats
        style = """
            QLabel {
                font-size: 18px; 
                font-weight: bold; 
                color: #333; 
                padding: 10px; 
                background: #e0e0e0; 
                border-radius: 5px;
                border: 1px solid #ccc;
            }
        """
        for lbl in [self.lbl_hmin, self.lbl_r]:
            lbl.setStyleSheet(style)
            stats_layout.addWidget(lbl)
        
        stats_layout.addStretch()
        layout.addLayout(stats_layout)

        # --- Controls Section ---
        controls_layout = QHBoxLayout()
        
        self.btn_manual = QPushButton("Plot Manual Data")
        self.btn_manual.clicked.connect(self.plot_manual)
        
        self.spin_runs = QSpinBox()
        self.spin_runs.setRange(1, 1000)
        self.spin_runs.setValue(10)
        self.spin_runs.setPrefix("Runs: ")
        
        self.btn_burst = QPushButton("Go")
        self.btn_burst.clicked.connect(self.start_burst)
        
        self.btn_cont = QPushButton("Start Continuous")
        self.btn_cont.setCheckable(True)
        self.btn_cont.clicked.connect(self.toggle_continuous)
        
        controls_layout.addWidget(self.btn_manual)
        controls_layout.addStretch()
        controls_layout.addWidget(self.spin_runs)
        controls_layout.addWidget(self.btn_burst)
        controls_layout.addWidget(self.btn_cont)
        
        layout.addLayout(controls_layout)
        
        # --- Visualization Section ---
        layout.addWidget(QLabel("<b>Geometric Interpretation (Byte Map):</b>"))
        self.viz = VisualizerWidget()
        layout.addWidget(self.viz, 1) # Stretch factor 1 to fill space
        
        # --- Raw Data Section ---
        layout.addWidget(QLabel("<b>Raw Hex Data:</b>"))
        self.txt_raw = QTextEdit()
        self.txt_raw.setMaximumHeight(80)
        self.txt_raw.setReadOnly(False)
        self.txt_raw.setStyleSheet("font-family: Monospace; font-size: 11px; color: #555; background: #f9f9f9;")
        layout.addWidget(self.txt_raw)

        # --- Serial Thread ---
        self.thread = SerialWorker()
        self.thread.data_received.connect(self.on_data)
        self.thread.start()
        
        # State
        self.continuous = False
        self.burst_remaining = 0
        
        # Initialize holding pattern
        default_hex = "c4e441871b5f1dd50dd7915b89c1733fcc62f548f453545ee48d59d63a9dbedc8f75685116a81a72cb02fec716770278118765126f63281ff2a5c9aab755ced927db67e613d96552e8febee2cf19bddd2ec2cccb543055ed3159287d30de38aa7fb01bae71ba02c326502010ead442263c18aecb1fa2c87aa1c1d5883d1c3b6e"
        self.txt_raw.setText(default_hex)
        self.plot_manual()

    def plot_manual(self):
        data = self.txt_raw.toPlainText().replace('\n', '').replace(' ', '')
        # self.viz.update_data(data)
        self.viz.update_data(logeaux)

    def start_burst(self):
        self.burst_remaining = self.spin_runs.value()
        self.continuous = False
        self.btn_cont.setChecked(False)
        self.btn_cont.setText("Start Continuous")

    def toggle_continuous(self, checked):
        self.continuous = checked
        if checked:
            self.btn_cont.setText("Stop Continuous")
            self.burst_remaining = 0
        else:
            self.btn_cont.setText("Start Continuous")

    def on_data(self, line):
        """Parses the incoming serial line and updates UI."""
        if not self.continuous and self.burst_remaining <= 0:
            return
            
        if self.burst_remaining > 0:
            self.burst_remaining -= 1

        # Expected format: H_min: 7.6781 | R: 1554 | Data: c4e4...
        try:
            parts = line.split('|')
            if len(parts) >= 3:
                # Extract values using split (more robust than fixed indices if whitespace varies)
                h_part = parts[0].split(':')[1].strip()
                r_part = parts[1].split(':')[1].strip()
                data_part = parts[2].split(':')[1].strip()
                
                # Update UI
                self.lbl_hmin.setText(f"H_min: {h_part}")
                self.lbl_r.setText(f"R: {r_part}")
                self.txt_raw.setText(data_part)
                
                # Update Visualization
                self.viz.update_data(data_part)
        except Exception as e:
            # Silently ignore parse errors for partial lines
            pass

    def closeEvent(self, event):
        """Clean up thread on close."""
        self.thread.requestInterruption()
        self.thread.wait()
        event.accept()

if __name__ == "__main__":
    app = QApplication(sys.argv)
    
    # Set a clean fusion style
    app.setStyle("Fusion")
    
    window = MainWindow()
    window.show()
    sys.exit(app.exec())
