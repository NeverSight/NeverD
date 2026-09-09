import fixture.FloatRounding;

public class RoundingHarness {
    private static void emit(String key, int value) { System.out.println(key + "=" + value); }
    public static void main(String[] arguments) {
        emit("rounding-field", Float.floatToRawIntBits(FloatRounding.field));
        emit("rounding-scalar", Float.floatToRawIntBits(FloatRounding.scalar()));
        float[] values = FloatRounding.array();
        emit("rounding-length", values.length);
        for (int i = 0; i < values.length; i++)
            emit("rounding-array:" + i, Float.floatToRawIntBits(values[i]));
    }
}
