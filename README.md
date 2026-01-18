# 🌐 PS5 Web-Based File Manager + System Monitor

**By Manos**

A powerful web-based file manager and system monitor for PS5 with etaHEN.

## ✨ Features

### 📁 File Manager
- **Browse filesystem** - Navigate all PS5 directories
- **Smart sorting** - Directories first, then files alphabetically
- **📤 Upload files** - Upload files from your computer to PS5 with progress bar
- **⬇️ Download files** - Download any file with zero-copy sendfile() optimization
- **Rename files** - Rename files and folders
- **Copy/Move files** - Copy or move files between directories
- **Delete files/folders** - Remove files and directories
- **Real-time updates** - See changes instantly
- **Modern UI** - Clean, responsive design with progress bars
- **Cross-platform** - Access from any device with a browser

### 📊 System Monitor
- **Storage info** - /data and /system storage usage (real-time)
- **RAM usage** - Actual memory usage via sysctl (real-time)
- **System uptime** - Actual system boot time (not payload time)
- **Network info** - IP address and hostname (real-time)
- **Server stats** - Total requests, files transferred, data transferred
- **Manual refresh** - No auto-refresh spam, click to update

## 🚀 How to Use

### 1. Compile
```bash
# On Windows with WSL
wsl -d Ubuntu-22.04 bash "/mnt/c/Users/HACKMAN/Desktop/ps5 test/ps5_rom_keys/ps5_web_manager/compile.sh"
```

### 2. Upload to PS5
- Copy `ps5_web_manager.elf` to `/data/etaHEN/payloads/`
- Use FTP or USB

### 3. Run on PS5
- Load the payload with elfldr
- You'll see a notification: `Web Manager: http://192.168.0.160:8080 - By Manos`

### 4. Access Web Interface
- Open browser on any device (PC, phone, tablet)
- Go to: `http://YOUR_PS5_IP:8080`
- Enjoy!

## 📱 Supported Devices

Access from:
- ✅ Windows PC
- ✅ Mac
- ✅ Linux
- ✅ Android phone/tablet
- ✅ iPhone/iPad
- ✅ Any device with a web browser!

## 🎯 Usage Examples

### File Manager
1. **Browse directories** - Click on folders to navigate
2. **Upload files** - Click "📤 Upload File" button, select file, watch progress bar
3. **Download files** - Click "⬇️ Download" button or click on file name
4. **Rename** - Click "Rename" button, enter new name
5. **Copy/Move** - Click "Copy" or "Move", select destination folder
6. **Delete items** - Click "Delete" button
7. **Go up** - Click "⬆️ Up" button
8. **Manual navigation** - Type path and click "Go"

### System Monitor
- View storage usage
- Check free space
- Monitor system uptime
- Auto-refreshes every 5 seconds

## 🔧 Technical Details

### Backend
- **Language**: C
- **SDK**: PS5 Payload SDK v0.35
- **HTTP Server**: Custom multi-threaded implementation
- **Port**: 8080
- **Buffer Size**: 1MB (optimized for large files)
- **Multi-threaded**: Yes (pthread)
- **REST API**: JSON responses

### Performance Optimizations
- **Zero-copy downloads**: sendfile() for 30-50% faster transfers
- **1MB buffers**: 16x larger than v1.0 for better throughput
- **TCP optimizations**: SO_NOSIGPIPE, TCP_NOPUSH, TCP_NODELAY
- **Connection timeouts**: 30s receive, 60s send
- **Smart file sorting**: qsort() with directories-first algorithm

### Frontend
- **Pure HTML/CSS/JavaScript** - No dependencies
- **Responsive design** - Works on all screen sizes
- **Dark theme** - Easy on the eyes
- **AJAX** - Async operations
- **Manual refresh** - No request spam

### API Endpoints
- `GET /` - Web interface
- `GET /api/list?path=<path>` - List directory contents (sorted)
- `GET /api/download?path=<path>` - Download file (sendfile optimized)
- `POST /api/upload?path=<path>` - Upload file (multipart/form-data)
- `GET /api/rename?old=<path>&new=<path>` - Rename file/directory
- `GET /api/copy?src=<path>&dst=<path>` - Copy file
- `GET /api/delete?path=<path>` - Delete file/directory
- `GET /api/sysinfo` - System information (real-time)

## 📊 Performance

- **Download speed**: 30-50% faster with sendfile() zero-copy
- **Buffer size**: 1MB (vs 64KB in v1.0)
- **Response times**: Optimized C backend with TCP tuning
- **Memory usage**: Efficient with connection timeouts
- **Concurrent connections**: Multiple users supported
- **Large files**: Handles files of any size with progress tracking

## 🛡️ Security Notes

- **Local network only** - Not exposed to internet
- **No authentication** - Trust your local network
- **Full filesystem access** - Be careful with delete operations

## 🎨 Screenshots

### 📊 System Monitor
![File Manager](screenshots/file-manager.png?raw=true)

### 📁 File Manager
![System Monitor](screenshots/system-monitor.png?raw=true)

## 🔄 Updates

### Version 2.0 (January 18, 2026)

#### 🚀 Major Performance Improvements
- **1MB Buffer Size** (upgraded from 64KB) - 16x larger for better throughput
- **Zero-Copy Downloads** - sendfile() optimization for 30-50% faster transfers
- **TCP Optimizations** - SO_NOSIGPIPE, TCP_NOPUSH, TCP_NODELAY for optimal performance

#### ✨ New Features
- **Smart File Sorting** - Directories always first, then files alphabetically
- **Real System Uptime** - Shows actual system boot time (not payload start time)
- **Real RAM Usage** - Uses sysctl to get actual memory stats (not estimates)
- **Manual Refresh** - No more auto-refresh spam in System Monitor
- **Connection Timeouts** - 30s receive, 60s send for better stability

#### 🔧 Technical Improvements
- Better error handling with detailed errno messages
- Improved memory detection with multiple sysctl methods
- Optimized socket options for downloads
- Directory listing with qsort() algorithm
- Fallback mechanisms for all system calls

#### 📊 Performance
- **Download Speed**: 30-50% faster with sendfile()
- **Buffer Size**: 1MB (16x improvement)
- **SDK**: PS5 Payload SDK v0.35
- **Compiler**: prospero-clang with -O3 optimization

### Version 1.0 (Initial Release)
- ✅ File browsing
- ✅ File upload with progress bar
- ✅ File download
- ✅ File/directory rename
- ✅ File copy/move
- ✅ File/directory deletion
- ✅ System info (storage, RAM, uptime, network)
- ✅ Server statistics
- ✅ Modern web UI with modal dialogs
- ✅ Multi-threaded server

### Future Features (Planned)
- 📝 Text file editor
- 🔍 File search
- 📦 Archive support (zip/tar)
- 📊 CPU/GPU monitoring
- 🔐 Optional authentication
- 🎨 Theme customization
- 📁 Batch operations

## 🐛 Troubleshooting

### Can't access web interface
- Check PS5 IP address in notification
- Make sure PS5 and device are on same network
- Try different browser

### Files not showing
- Check directory permissions
- Try navigating to /data first

### Download not working
- Check file permissions
- Try smaller files first

## 📝 License

Free to use and modify for the PS5 homebrew community!

## 👨‍💻 Author

**By Manos**

Created for the PS5 homebrew community with ❤️

## 🙏 Credits

- PS5 SDK
- etaHEN
- PS5 homebrew community

---

**Enjoy your PS5 Web Manager!** 🚀
