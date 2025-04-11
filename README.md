# VMAVIFicient – AV1/AVIF Transcoder

**VMAVIFicient** is a standalone AV1/AVIF transcoder written in C. It converts videos to AV1 with Opus audio in MKV containers and images to AVIF (AV1 Image File Format). This project is designed to use CMake with the Ninja build system for managing dependencies and building the project, ensuring that it never relies on system packages.

## Key Features

- **Video Transcoding to AV1**: Utilizes AOMedia's AV1 codec for video encoding and Opus codec for audio, all packaged in MKV containers.
- **Image Conversion to AVIF**: Converts images in formats like PNG and JPEG to AVIF with high fidelity.
- **Quality Assurance with VMAF**: Optionally integrates Netflix's VMAF metric to assess video quality.
- **Metadata Preservation**: Maintains image metadata during conversion using `libexif`.
- **Logging & Reporting**: Logs important details such as original vs. new file sizes, size reduction percentages, and encoding settings.

## Project Structure

```plaintext
vmavificient/
├── CMakeLists.txt            # Top-level CMake file
├── src/                      
│   ├── main.c                # CLI handling
│   ├── video/                # Video transcoding
│   │   ├── CMakeLists.txt    # CMake file for the video module
│   │   ├── video.c           # Video source code
│   │   └── video.h           # Header for video module
│   ├── audio/                # Audio transcoding
│   │   ├── CMakeLists.txt    # CMake file for the audio module
│   │   ├── audio.c           # Audio source code
│   │   └── audio.h           # Header for audio module
│   ├── image/                # Image conversion to AVIF
│   │   ├── CMakeLists.txt    # CMake file for the image module
│   │   ├── image.c           # Image source code
│   │   └── image.h           # Header for image module
│   ├── vmaf/                 # VMAF integration
│   │   ├── CMakeLists.txt    # CMake file for the VMAF module
│   │   ├── vmaf.c            # VMAF source code
│   │   └── vmaf.h            # Header for VMAF module
│   └── metadata/             # Metadata handling
│       ├── CMakeLists.txt    # CMake file for the metadata module
│       ├── metadata.c        # Metadata source code
│       └── metadata.h        # Header for metadata module
├── external/                 # External libraries (using CMake FetchContent)
│   ├── FFmpeg/               # FFmpeg source
│   ├── libaom/               # AOMedia AV1 codec source
│   ├── libopus/              # Opus codec source
│   ├── libvmaf/              # VMAF library source
│   └── libexif/              # libexif source
├── VERSION                   # Project version
├── README.md                 # Project documentation
└── LICENSE                   # BSD 2-Clause License text
```

## CMake Configuration

### Dependencies

The project uses CMake's `FetchContent` module to download and build the required dependencies, ensuring that it remains fully standalone.

### Building the Project

To build the project, follow these steps:

1. **Clone the repository**:
    ```bash
    git clone --recurse-submodules <repository-url>
    cd vmavificient
    ```

2. **Create a build directory**:
    ```bash
    mkdir build
    cd build
    ```

3. **Run CMake to configure the project**:
    ```bash
    cmake -G Ninja ..
    ```

4. **Build the project**:
    ```bash
    ninja
    ```

## Usage

After building, you can use the `vmavificient` executable located in the build directory to transcode videos or convert images.

### Transcode a Video

```bash
./vmavificient input.mp4 output.mkv
```

### Convert an Image

```bash
./vmavificient photo.png photo.avif
```

### Disable VMAF for Speed

```bash
./vmavificient --no-vmaf input.mkv output.mkv
```

## Logging and Results Interpretation

The program logs details about the conversion process, including:

```
Transcode complete: original size 50 MB, new size 30 MB (40% reduction).
```

## License

This project is licensed under the BSD 2-Clause License. See the `LICENSE` file for details.

## Contribution

Feel free to open issues or submit pull requests for enhancements or bug fixes.

Thank you for your interest in **VMAVIFicient**! We hope you find it a useful tool for your transcoding needs.
