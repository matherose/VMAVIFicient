Certainly! Below is an updated version of the README written in raw markdown for the **VMAVIFicient** project, focusing on using Meson build system with `wrap-git` for dependency management (instead of using submodules).

```markdown
# VMAVIFicient – AV1/AVIF Transcoder

**VMAVIFicient** is a standalone AV1/AVIF transcoder written in C that converts videos to AV1 with Opus audio in MKV containers and images to AVIF (AV1 Image File Format). The project is designed to use the Meson build system for building and managing external library dependencies.

## Key Features

- **Video Transcoding to AV1**: Utilizes AOMedia's AV1 codec for video encoding and Opus codec for audio, packaged in MKV containers.
- **Image Conversion to AVIF**: Converts images (PNG, JPEG, etc.) to AVIF with high fidelity.
- **Quality Assurance with VMAF**: Optionally integrates Netflix's VMAF metric to assess video quality.
- **Metadata Preservation**: Maintains image metadata during conversion using `libexif`.
- **Logging & Reporting**: Logs details such as original vs. new file sizes, achieved size reduction, and encoding adjustments.

## Project Structure

```plaintext
vmavificient/
├── meson.build              # Top-level Meson build file
├── src/                    
│   ├── main.c              # CLI handling
│   ├── video/              # Video transcoding
│   │   ├── video.c         # Video source code
│   │   └── video.h         # Header for video module
│   ├── audio/              # Audio transcoding
│   │   ├── audio.c         # Audio source code
│   │   └── audio.h         # Header for audio module
│   ├── image/              # Image conversion to AVIF
│   │   ├── image.c         # Image source code
│   │   └── image.h         # Header for image module
│   ├── vmaf/               # VMAF integration
│   │   ├── vmaf.c          # VMAF source code
│   │   └── vmaf.h          # Header for VMAF module
│   └── metadata/           # Metadata handling
│       ├── metadata.c      # Metadata source code
│       └── metadata.h      # Header for metadata module
├── wrap/                   # Directory for Meson wrap files
│   ├── FFmpeg.wrap         # Wrap file for FFmpeg
│   ├── libaom.wrap         # Wrap file for AOMedia AV1 codec
│   ├── libopus.wrap        # Wrap file for Opus codec
│   ├── libvmaf.wrap        # Wrap file for VMAF library
│   └── libexif.wrap        # Wrap file for libexif
├── VERSION                 # Project version
├── README.md               # Project documentation
└── LICENSE                 # BSD 2-Clause License text
```

## Meson Build Configuration

### Setting Up the Dependencies

To manage dependencies, Meson uses `wrap-git`. You can create the following `.wrap` files in the `wrap/` directory for your required libraries:

- **FFmpeg.wrap**
- **libaom.wrap**
- **libopus.wrap**
- **libvmaf.wrap**
- **libexif.wrap**

### Example Wrap File (libexif.wrap)

Here’s an example of what your `libexif.wrap` could look like:

```ini
[wrap-git]
directory = libexif
url = git://git.savannah.nongnu.org/libexif.git
revision = master
```

You can create similar wrap files for the other libraries.

### Configuring and Building the Project

To build the project, follow these steps:

1. **Clone the repository**:
    ```bash
    git clone <repository-url>
    cd vmavificient
    ```

2. **Create a build directory**:
    ```bash
    meson setup builddir
    ```

3. **Build the project**:
    ```bash
    meson compile -C builddir
    ```

## Usage

Once built, you can use the `vmavificient` binary located in the build directory to transcode videos or convert images.

### Transcode a Video

```bash
./builddir/vmavificient input.mp4 output.mkv
```

### Convert an Image

```bash
./builddir/vmavificient photo.png photo.avif
```

### Disable VMAF for Speed

```bash
./builddir/vmavificient --no-vmaf input.mkv output.mkv
```

## Logging and Results Interpretation

The program logs details about the conversion, such as:

```
Transcode complete: original size 50 MB, new size 30 MB (40% reduction).
```

## License

This project is licensed under the BSD 2-Clause License managed by SPDX. See the `LICENSE` file for details.

## Library versions
- **FFmpeg**: n7.1.1
- **libaom**: n3.12.0
- **libopus**: n1.5.2
- **libvmaf**: v3.0.0
- **libexif**: v0.6.25

## Contribution

Feel free to open issues or submit pull requests for enhancements or bug fixes.

Thank you for your interest in **VMAVIFicient**! We hope you find it a useful tool for transcoding needs.
