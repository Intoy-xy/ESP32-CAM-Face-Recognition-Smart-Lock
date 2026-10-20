# Laptop application notes

The Windows laptop runs the face enrollment and recognition program. The ESP32-CAM supplies the MJPEG camera stream and controls the solenoid. This laptop component is required for face unlock; the ESP32-CAM does not perform the face matching by itself.

See the repository [README](../README.md) for the complete setup, local configuration, and run commands. Face profiles are stored in `face_data.npz` beside this script or beside the packaged executable and are excluded from Git.
