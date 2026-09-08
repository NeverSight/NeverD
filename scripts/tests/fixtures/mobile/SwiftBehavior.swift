// Self-contained source corpus for native-to-Swift behavioral verification.
@inline(never) public func scalar(_ x: Int64, _ y: Int64) -> Int64 { x &+ y }
@inline(never) public func choose(_ x: Int64, _ y: Int64) -> Int64 { x < y ? x : y }
@inline(never) public func callScalar(_ x: Int64, _ y: Int64) -> Int64 { scalar(x, y) &* 3 }

@inline(never) public func floatIdentity(_ x: Float) -> Float { x }
@inline(never) public func doubleIdentity(_ x: Double) -> Double { x }
@inline(never) public func floatAdd(_ x: Float, _ y: Float) -> Float { x + y }
@inline(never) public func doubleAdd(_ x: Double, _ y: Double) -> Double { x + y }
@inline(never) public func mixed(_ x: Int8, _ y: Double, _ z: Int64, _ w: Float) -> Double {
    Double(x) + y + Double(z) + Double(w)
}
@inline(never) public func stackIntegers(
    _ a: Int8, _ b: Int16, _ c: Int32, _ d: Int64,
    _ e: Int8, _ f: Int16, _ g: Int32, _ h: Int64,
    _ i: Int8, _ j: Int16, _ k: Int32, _ l: Int64
) -> Int64 {
    Int64(a) &+ Int64(b) &+ Int64(c) &+ d &+ Int64(e) &+ Int64(f) &+
    Int64(g) &+ h &+ Int64(i) &+ Int64(j) &+ Int64(k) &+ l
}
@inline(never) public func stackFloats(
    _ a: Float, _ b: Float, _ c: Float, _ d: Float, _ e: Float, _ f: Float,
    _ g: Float, _ h: Float, _ i: Float, _ j: Float, _ k: Float, _ l: Float
) -> Float { a + b + c + d + e + f + g + h + i + j + k + l }

@inline(never) public func stackMixed(
    _ a: Int64, _ b: Int64, _ c: Int64, _ d: Int64,
    _ e: Int64, _ f: Int64, _ g: Int64, _ h: Int64,
    _ p: Double, _ q: Double, _ r: Double, _ s: Double,
    _ t: Double, _ u: Double, _ v: Double, _ w: Double,
    _ i: Int8, _ j: Float, _ k: Int16, _ l: Float,
    _ m: Int32, _ n: Double, _ o: Int64
) -> Double {
    let integers = a &+ b &+ c &+ d &+ e &+ f &+ g &+ h
    let floats = p + q + r + s + t + u + v + w
    return Double(integers) + floats + Double(i) + Double(j) + Double(k) + Double(l) + Double(m) + n + Double(o)
}

@inline(never) public func pointerRead(_ pointer: UnsafePointer<Int32>) -> Int32 { pointer.pointee }
@inline(never) public func pointerSwap(_ pointer: UnsafeMutablePointer<Int32>, _ value: Int32) -> Int32 {
    let old = pointer.pointee
    pointer.pointee = value
    return old
}
@inline(never) public func sum(_ pointer: UnsafePointer<Int32>, _ count: Int64) -> Int64 {
    var result: Int64 = 0
    var index: Int64 = 0
    while index < count {
        result &+= Int64(pointer[Int(index)])
        index &+= 1
    }
    return result
}

public final class Calculator {
    public let bias: Int64
    public init(_ bias: Int64) { self.bias = bias }
    @inline(never) public func add(_ value: Int64) -> Int64 { value &+ bias }
    @inline(never) public func selfCall(_ value: Int64) -> Int64 { add(value) &- 5 }
    @inline(never) public static func combine(_ x: Int64, _ y: Int64) -> Int64 { x &- y }
}

@frozen public struct Counter {
    public var value: Int64
    public init(_ value: Int64) { self.value = value }
    @inline(never) public func add(_ delta: Int64) -> Int64 { value &+ delta }
    @inline(never) public func affine(_ delta: Int64) -> Int64 { (value &* 7) &+ delta }
    @inline(never) public mutating func adjust(_ delta: Int64) { value = (value &* 7) &+ delta }
}
