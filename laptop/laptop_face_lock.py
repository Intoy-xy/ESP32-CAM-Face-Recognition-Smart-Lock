"""Windows laptop face enrollment/recognition client for the ESP32-CAM lock."""

from __future__ import annotations

import argparse
import json
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path

import cv2
import numpy as np


try:
    from local_config import ESP_BASE_URL, LAPTOP_UNLOCK_TOKEN
except ImportError as exc:
    raise SystemExit("Copy local_config.example.py to local_config.py and set the local connection values.") from exc


APP_DIR = Path(sys.executable).resolve().parent if getattr(sys, "frozen", False) else Path(__file__).resolve().parent
DATA_FILE = APP_DIR / "face_data.npz"
FACE_SIZE = (160, 160)
MAX_PROFILES = 3
SAMPLES_PER_ENROLLMENT = 8
SAMPLE_INTERVAL_SECONDS = 0.2
MAX_LBPH_DISTANCE = 70.0
REQUIRED_CONSECUTIVE_MATCHES = 3


def get_status(base_url: str) -> str | None:
    request = urllib.request.Request(f"{base_url}/status", method="GET")
    with urllib.request.urlopen(request, timeout=2) as response:
        status = json.loads(response.read().decode("utf-8"))
    return status.get("state")


def mjpeg_frames(stream_url: str):
    """Yield JPEG frames from the ESP32 multipart stream, reconnecting on drop."""
    while True:
        try:
            request = urllib.request.Request(stream_url, method="GET")
            with urllib.request.urlopen(request, timeout=8) as response:
                content_type = response.headers.get("Content-Type", "")
                boundary = content_type.split("boundary=", 1)[-1].strip().strip('"')
                if not boundary:
                    raise RuntimeError("ESP32 stream did not provide a multipart boundary")
                marker = b"--" + boundary.encode("ascii")

                while True:
                    line = response.readline()
                    if not line:
                        raise ConnectionError("ESP32 video stream ended")
                    if line.strip() != marker:
                        continue

                    headers: dict[str, str] = {}
                    while True:
                        line = response.readline()
                        if not line or line in (b"\r\n", b"\n"):
                            break
                        name, separator, value = line.decode("ascii", "ignore").partition(":")
                        if separator:
                            headers[name.strip().lower()] = value.strip()

                    frame_length = int(headers.get("content-length", "0"))
                    if frame_length <= 0:
                        raise RuntimeError("ESP32 stream frame is missing Content-Length")
                    frame = response.read(frame_length)
                    if len(frame) != frame_length:
                        raise ConnectionError("Incomplete JPEG frame received")
                    yield frame
        except (OSError, urllib.error.URLError, ValueError, RuntimeError) as error:
            print(f"Camera stream unavailable ({error}); retrying in 2 seconds...")
            time.sleep(2)


def load_profiles():
    if not DATA_FILE.exists():
        return [], [], []

    with np.load(DATA_FILE, allow_pickle=False) as saved:
        faces = [image.copy() for image in saved["faces"]]
        labels = [int(label) for label in saved["labels"]]
        names = [str(name) for name in saved["names"].tolist()]
    return faces, labels, names


def save_profiles(faces, labels, names):
    DATA_FILE.parent.mkdir(parents=True, exist_ok=True)
    temporary = DATA_FILE.with_suffix(".tmp")
    with temporary.open("wb") as output:
        np.savez_compressed(
            output,
            faces=np.asarray(faces, dtype=np.uint8),
            labels=np.asarray(labels, dtype=np.int32),
            names=np.asarray(names, dtype=np.str_),
        )
    temporary.replace(DATA_FILE)


def make_face_cascade():
    cascade_path = Path(cv2.data.haarcascades) / "haarcascade_frontalface_default.xml"
    cascade = cv2.CascadeClassifier(str(cascade_path))
    if cascade.empty():
        raise RuntimeError(f"Could not load OpenCV face detector: {cascade_path}")
    return cascade


def detect_face(frame, cascade):
    gray = cv2.cvtColor(frame, cv2.COLOR_BGR2GRAY)
    found = cascade.detectMultiScale(
        gray,
        scaleFactor=1.1,
        minNeighbors=5,
        minSize=(55, 55),
    )
    if len(found) == 0:
        return gray, None, None

    x, y, width, height = max(found, key=lambda box: int(box[2]) * int(box[3]))
    crop = gray[y : y + height, x : x + width]
    crop = cv2.resize(crop, FACE_SIZE, interpolation=cv2.INTER_AREA)
    crop = cv2.equalizeHist(crop)
    return gray, (int(x), int(y), int(width), int(height)), crop


def wait_for_state(base_url: str, wanted_state: str):
    print(f"Waiting for keypad state: {wanted_state}")
    last_message = 0.0
    while True:
        try:
            state = get_status(base_url)
            if state == wanted_state:
                print("Device is ready. Press Q in the preview window to cancel.")
                return
            now = time.monotonic()
            if now - last_message >= 5:
                print(f"Current device state: {state or 'unavailable'}; select the matching keypad option.")
                last_message = now
        except (OSError, urllib.error.URLError, json.JSONDecodeError) as error:
            now = time.monotonic()
            if now - last_message >= 5:
                print(f"Cannot reach ESP32 at {base_url} ({error}); connect to SmartLock-Cam.")
                last_message = now
        time.sleep(0.5)


def enroll(base_url: str, stream_url: str, name: str):
    name = name.strip()
    if not name or len(name) > 32:
        raise ValueError("Name must contain 1 to 32 characters")

    faces, labels, names = load_profiles()
    matching_name = next((item for item in names if item.casefold() == name.casefold()), None)
    if matching_name is None and len(names) >= MAX_PROFILES:
        raise ValueError(f"The local database is limited to {MAX_PROFILES} profiles")
    if matching_name is None:
        names.append(name)
        profile_label = len(names)
    else:
        profile_label = names.index(matching_name) + 1
        name = matching_name

    wait_for_state(base_url, "ENROLLING FACE")
    cascade = make_face_cascade()
    print(f"Enrolling {name}. Slowly turn your head a little between samples.")

    collected = 0
    next_sample = 0.0
    last_status_check = 0.0
    try:
        for jpeg in mjpeg_frames(stream_url):
            now = time.monotonic()
            if now - last_status_check >= 0.5:
                last_status_check = now
                try:
                    state = get_status(base_url)
                except (OSError, urllib.error.URLError, json.JSONDecodeError):
                    state = None
                if state is not None and state != "ENROLLING FACE":
                    print("Enrollment canceled because the keypad left enrollment mode.")
                    break

            frame = cv2.imdecode(np.frombuffer(jpeg, dtype=np.uint8), cv2.IMREAD_COLOR)
            if frame is None:
                continue
            _, box, face = detect_face(frame, cascade)

            message = f"Enrollment samples: {collected}/{SAMPLES_PER_ENROLLMENT}"
            if box is not None:
                x, y, width, height = box
                cv2.rectangle(frame, (x, y), (x + width, y + height), (0, 220, 0), 2)
                if now >= next_sample and collected < SAMPLES_PER_ENROLLMENT:
                    faces.append(face)
                    labels.append(profile_label)
                    collected += 1
                    next_sample = now + SAMPLE_INTERVAL_SECONDS
                    message = f"Captured {collected}/{SAMPLES_PER_ENROLLMENT}"
            else:
                message += " - center your face"

            cv2.putText(frame, message, (8, 20), cv2.FONT_HERSHEY_SIMPLEX, 0.55, (0, 220, 0), 2)
            cv2.imshow("Smart Lock Face Enrollment", frame)
            if cv2.waitKey(1) & 0xFF == ord("q"):
                print("Enrollment canceled; captured samples were not saved.")
                return
            if collected >= SAMPLES_PER_ENROLLMENT:
                break
    finally:
        cv2.destroyAllWindows()

    if collected < SAMPLES_PER_ENROLLMENT:
        print("Enrollment incomplete; no changes were saved.")
        return

    save_profiles(faces, labels, names)
    print(f"Saved {collected} samples for {name} in {DATA_FILE}")
    print("Press D on the keypad to leave face enrollment mode.")


def send_unlock(base_url: str):
    request = urllib.request.Request(
        f"{base_url}/unlock",
        data=b"",
        headers={"X-Lock-Token": LAPTOP_UNLOCK_TOKEN},
        method="POST",
    )
    with urllib.request.urlopen(request, timeout=3) as response:
        return response.status, response.read().decode("utf-8", "replace")


def diagnose_unlock_failure(base_url: str, error: Exception):
    """Check whether the control server is still reachable after a failed POST."""
    print(f"Unlock request failed ({type(error).__name__}): {error}")
    try:
        state = get_status(base_url)
    except (OSError, urllib.error.URLError, json.JSONDecodeError) as status_error:
        print(
            "The ESP32 status page also did not respond. Check that this laptop is "
            "connected to SmartLock-Cam and that the board is still powered. "
            f"Status check: {status_error}"
        )
    else:
        print(
            f"The ESP32 is still responding to /status (state: {state or 'unknown'}), "
            "so Wi-Fi is up. The /unlock handler closed its connection before "
            "returning an HTTP response."
        )


def run_recognition(base_url: str, stream_url: str):
    faces, labels, names = load_profiles()
    if not faces or not names:
        raise RuntimeError("No faces are enrolled yet. Choose Enroll from the app menu first.")

    recognizer = cv2.face.LBPHFaceRecognizer_create()
    recognizer.train(faces, np.asarray(labels, dtype=np.int32))
    cascade = make_face_cascade()
    print(f"Loaded {len(names)} profile(s). Waiting for keypad face-unlock mode.")
    last_status_check = 0.0
    active_scan = False
    matching_label = None
    consecutive_matches = 0
    unlock_sent = False

    try:
        while True:
            now = time.monotonic()
            if now - last_status_check >= 0.5:
                last_status_check = now
                try:
                    state = get_status(base_url)
                except (OSError, urllib.error.URLError, json.JSONDecodeError):
                    state = None

                scanning = state == "SCANNING FACE"
                if scanning and not active_scan:
                    print("Face scan active; press D to cancel on the keypad.")
                if not scanning and active_scan:
                    print("Face scan ended. Waiting for the next keypad request.")
                    cv2.destroyAllWindows()
                    matching_label = None
                    consecutive_matches = 0
                    unlock_sent = False
                active_scan = scanning

            if not active_scan:
                time.sleep(0.1)
                continue

            for jpeg in mjpeg_frames(stream_url):
                now = time.monotonic()
                if now - last_status_check >= 0.5:
                    last_status_check = now
                    try:
                        state = get_status(base_url)
                    except (OSError, urllib.error.URLError, json.JSONDecodeError):
                        state = None
                    if state != "SCANNING FACE":
                        active_scan = False
                        matching_label = None
                        consecutive_matches = 0
                        unlock_sent = False
                        cv2.destroyAllWindows()
                        break

                frame = cv2.imdecode(np.frombuffer(jpeg, dtype=np.uint8), cv2.IMREAD_COLOR)
                if frame is None:
                    continue
                _, box, face = detect_face(frame, cascade)
                label_text = "No face detected"

                if box is not None:
                    x, y, width, height = box
                    predicted_label, distance = recognizer.predict(face)
                    matched = distance <= MAX_LBPH_DISTANCE
                    if matched and 1 <= predicted_label <= len(names):
                        if matching_label == predicted_label:
                            consecutive_matches += 1
                        else:
                            matching_label = predicted_label
                            consecutive_matches = 1
                        label_text = f"{names[predicted_label - 1]} ({distance:.0f})"
                        color = (0, 220, 0)
                    else:
                        matching_label = None
                        consecutive_matches = 0
                        label_text = f"Unknown ({distance:.0f})"
                        color = (0, 0, 220)

                    cv2.rectangle(frame, (x, y), (x + width, y + height), color, 2)

                    if (
                        consecutive_matches >= REQUIRED_CONSECUTIVE_MATCHES
                        and not unlock_sent
                    ):
                        try:
                            result, message = send_unlock(base_url)
                            print(f"ESP32 unlock response {result}: {message}")
                            unlock_sent = True
                        except urllib.error.HTTPError as error:
                            print(f"ESP32 rejected unlock: HTTP {error.code}")
                            unlock_sent = True
                        except (OSError, urllib.error.URLError) as error:
                            diagnose_unlock_failure(base_url, error)
                            unlock_sent = True

                cv2.putText(frame, label_text, (8, 20), cv2.FONT_HERSHEY_SIMPLEX, 0.6, (0, 220, 0), 2)
                cv2.imshow("Smart Lock Face Recognition", frame)
                if cv2.waitKey(1) & 0xFF == ord("q"):
                    return
    finally:
        cv2.destroyAllWindows()


def main():
    parser = argparse.ArgumentParser(description="Laptop-side face lock app")
    parser.add_argument("--esp", default=ESP_BASE_URL, help="ESP32 base URL (default: %(default)s)")
    parser.add_argument(
        "--stream",
        default="http://192.168.4.1:81/stream",
        help="ESP32 MJPEG URL (default: %(default)s)",
    )
    subparsers = parser.add_subparsers(dest="command")

    enroll_parser = subparsers.add_parser("enroll", help="Enroll a face while the keypad is in enrollment mode")
    enroll_parser.add_argument("--name", required=True, help="Profile name")
    subparsers.add_parser("run", help="Wait for keypad face-unlock requests")

    args = parser.parse_args()
    if args.command == "enroll":
        enroll(args.esp.rstrip("/"), args.stream, args.name)
    elif args.command == "run":
        run_recognition(args.esp.rstrip("/"), args.stream)
    else:
        interactive_app(args.esp.rstrip("/"), args.stream)


def interactive_app(base_url: str, stream_url: str):
    """Keep one double-clickable session open for enrollment and recognition."""
    while True:
        print("\nSmart Lock Face App")
        print("[E] Enroll a face")
        print("[R] Wait for keypad face unlock")
        print("[Q] Quit")
        choice = input("Choose an option: ").strip().casefold()
        if choice == "q":
            return
        if choice == "e":
            name = input("Name for this face (up to 32 characters): ").strip()
            try:
                enroll(base_url, stream_url, name)
            except (ValueError, RuntimeError) as error:
                print(f"Could not enroll: {error}")
            continue
        if choice == "r":
            try:
                run_recognition(base_url, stream_url)
            except RuntimeError as error:
                print(error)
            continue
        print("Choose E, R, or Q.")


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        print("Stopped.")
    except Exception as error:
        print(f"Error: {error}")
        raise SystemExit(1)
