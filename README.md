# opentype_cpp

OpenType fonts read: metrics, the `cmap` (formats 4 and 12), `kern`, and glyph outlines from TrueType `glyf` (quadratics) and CFF (Type 2 charstrings, cubics), in font units. Written from the OpenType specification and Adobe Technical Notes #5176 and #5177; the CFF container follows the structure of the SwiftOTF package.

C++20 with only the standard library. One of a family of small libraries built and used the same
way: [math_cpp](https://github.com/eliseyOzerov/math_cpp), xml_cpp, deflate_cpp, css_cpp and
opentype_cpp.

## Build and test

```bash
make test
```

builds `build/lib/libopentype_cpp.a` and runs the unit tests.

## Using it

**make or any build system:** put the folder that holds `opentype_cpp` on the include path, include
`"opentype_cpp/OpenType/<Header>.h"`, and link `libopentype_cpp.a`.

**SwiftPM:** the product is `OpenTypeCpp`.

```swift
.package(path: "../opentype_cpp"),
// ...
.target(name: "App", dependencies: [.product(name: "OpenTypeCpp", package: "opentype_cpp")],
        swiftSettings: [.interoperabilityMode(.Cxx)])
```

Everything is in the C++ namespace `editor`, as in math_cpp. Values come back in the format's own
terms; turning them into a host's types is the host's job.

## License

MIT, see [LICENSE](LICENSE).
