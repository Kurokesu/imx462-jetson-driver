import argparse
import numpy as np
import cv2
from pathlib import Path

def main():
    parser = argparse.ArgumentParser(
        description="Display a RAW frame from IMX462 sensor."
    )
    parser.add_argument(
        "file_path",
        type=Path,
        help="Path to the raw file (e.g., ./imx462_1080p.raw)"
    )
    parser.add_argument(
        "--mono",
        action="store_true",
        help="Mono sensor frame, skip debayer"
    )
    args = parser.parse_args()

    file_path = args.file_path.expanduser()
    W, H = 1920, 1080

    with file_path.open("rb") as f:
        data = np.fromfile(f, dtype=np.uint16).reshape((H, W))

    # Remove the 6 duplicated MSBs
    raw10 = (data >> 6).astype(np.uint16)

    img = raw10 if args.mono else cv2.cvtColor(raw10, cv2.COLOR_BayerRG2BGR)
    disp = cv2.convertScaleAbs(img, alpha=255.0/1023.0)

    cv2.imshow("Mono" if args.mono else "Bayer", disp)
    cv2.waitKey(0)
    cv2.destroyAllWindows()

if __name__ == "__main__":
    main()
