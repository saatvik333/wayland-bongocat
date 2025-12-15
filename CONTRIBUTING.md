# Contributing to Bongo Cat Wayland Overlay

Thank you for your interest in contributing! 🐱

## Getting Started

### Prerequisites

- Wayland compositor with layer shell support
- GCC or Clang (C23 support)
- wayland-client, wayland-protocols
- Make

### Building

```bash
git clone https://github.com/saatvik333/wayland-bongocat.git
cd wayland-bongocat
make debug    # Development build with debug symbols
make          # Release build
```

### Running

```bash
./build/bongocat -c bongocat.conf -w
```

## Development Workflow

### Code Style

- Run `make format` before committing
- Follow the existing code patterns
- Use meaningful variable names

### Making Changes

1. Fork the repository
2. Create a feature branch: `git checkout -b feature/my-feature`
3. Make your changes
4. Test thoroughly
5. Submit a pull request

### Commit Messages

Use conventional commits:

```
feat: add new feature
fix: resolve bug
docs: update documentation
refactor: improve code structure
```

## Code Structure

```
src/
├── core/       # Main application logic
├── config/     # Configuration parsing
├── graphics/   # Animation and rendering
├── platform/   # Wayland integration
└── utils/      # Error handling, memory
```

## Testing

```bash
# Run with debug logging
./build/bongocat -c bongocat.conf -w

# Check for memory leaks
make memcheck
```

## Reporting Issues

When reporting bugs, please include:

- Your compositor (Sway, Hyprland, etc.)
- Config file contents
- Debug output (`enable_debug=1`)

## Questions?

Open an issue or reach out to the maintainer.

Thanks for helping make Bongo Cat better! 🎉
