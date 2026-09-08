import fixture.Calculator;

public class LegacyHarness {
    private static void emit(String key, long value) { System.out.println(key + "=" + value); }
    public static void main(String[] arguments) {
        int[] values = {Integer.MIN_VALUE, -32769, -1, 0, 1, 32768, Integer.MAX_VALUE};
        for (int i = 0; i < values.length; i++) {
            emit("compute:" + i, Calculator.compute(values[i]));
            emit("nested:" + i, Calculator.Nested.bump(values[i]));
            for (int j = 0; j < values.length; j++) emit("divide:" + i + ":" + j, Calculator.safeDivide(values[i], values[j]));
        }
        int[][] arrays = {{}, {-3, 2, -9, 0}, {Integer.MIN_VALUE}, {Integer.MAX_VALUE, 1}, {-1, -2, -3, -4}};
        for (int i = 0; i < arrays.length; i++) emit("sum-abs:" + i, Calculator.sumAbs(arrays[i]));
        emit("greeting", fixture.Peer.greeting().equals("neverd") ? 1 : 0);
    }
}
