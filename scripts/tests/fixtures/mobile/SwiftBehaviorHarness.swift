#if ORIGINAL_FIXTURE
import SwiftBehavior
#endif

// This harness contains checks only. It supplies no recovered declarations,
// field layouts, initializers, or wrappers around the original binary.
@main struct VerifySwiftBehavior {
    static func emit<T>(_ name: String, _ index: Int, _ value: T) { print("\(name):\(index)=\(value)") }
    static func main() {
        let integers: [Int64] = [.min, .min + 1, -65537, -1, 0, 1, 65537, .max - 1, .max]
        for (xIndex, x) in integers.enumerated() {
            let calculator = Calculator(x)
            emit("class-init", xIndex, calculator.bias)
            emit("struct-init", xIndex, Counter(x).value)
            for (yIndex, y) in integers.enumerated() {
                let index = xIndex * integers.count + yIndex
                emit("scalar", index, scalar(x, y))
                emit("choose", index, choose(x, y))
                emit("callScalar", index, callScalar(x, y))
                emit("class-add", index, calculator.add(y))
                emit("class-selfCall", index, calculator.selfCall(y))
                emit("class-combine", index, Calculator.combine(x, y))
                var counter = Counter(x)
                emit("struct-add", index, counter.add(y))
                emit("struct-affine", index, counter.affine(y))
                counter.adjust(y)
                emit("struct-adjust", index, counter.value)
            }
        }
        let floatBits: [UInt32] = [0, 0x80000000, 0x7f800000, 0xff800000, 0x7fc12345, 1, 0xc0f00000]
        let doubleBits: [UInt64] = [0, 0x8000000000000000, 0x7ff0000000000000,
                                   0xfff0000000000000, 0x7ff8000000001234, 1, 0xc020800000000000]
        for (index, bits) in floatBits.enumerated() { emit("floatIdentity", index, floatIdentity(Float(bitPattern: bits)).bitPattern) }
        for (index, bits) in doubleBits.enumerated() { emit("doubleIdentity", index, doubleIdentity(Double(bitPattern: bits)).bitPattern) }
        for index in -3...3 {
            let suffix = index + 3
            emit("floatAdd", suffix, floatAdd(Float(index) * 2.25, -7.5).bitPattern)
            emit("doubleAdd", suffix, doubleAdd(Double(index) * 3.125, -1024.5).bitPattern)
            emit("mixed", suffix, mixed(Int8(index), -13.25, 10000000000, 2.5).bitPattern)
            emit("stackIntegers", suffix, stackIntegers(Int8(index), -32767, 65537, 10000000000,
                                                       -127, 32767, -65537, -10000000000, -126, -32766, 65536, 17))
            emit("stackFloats", suffix, stackFloats(Float(index), -3.25, 5.5, -7.75, 11.25, -13.5,
                                                   17.75, -19.25, 23.5, -29.75, 31.25, -37.5).bitPattern)
            emit("stackMixed", suffix, stackMixed(Int64(index), -3, 5, -7, 11, -13, 17, -19,
                                                  2.25, -3.5, 5.75, -7.25, 11.5, -13.75, 17.25, -19.5,
                                                  -127, 2.5, -32767, 3.25, 65537, -23.5, 10000000000).bitPattern)
        }
        let cells: [Int32] = [.min, -1, 0, 1, .max]
        for (index, input) in cells.enumerated() {
            var cell = input
            withUnsafeMutablePointer(to: &cell) { pointer in
                emit("pointerRead", index, pointerRead(UnsafePointer(pointer)))
                emit("pointerSwap", index, pointerSwap(pointer, cells[cells.count - index - 1]))
            }
            emit("pointerAfter", index, cell)
        }
        var array: [Int32] = [-5, 7, 0, 9, -3]
        for index: Int in 5..<35 {
            let value: Int32
            if index % 7 == 0 {
                value = Int32.min
            } else if index % 7 == 1 {
                value = Int32.max
            } else {
                value = Int32(index * index - 97)
            }
            array.append(value)
        }
        array.withUnsafeBufferPointer { buffer in
            for count in -1...array.count { emit("sum", count + 1, sum(buffer.baseAddress!, Int64(count))) }
        }
    }
}
