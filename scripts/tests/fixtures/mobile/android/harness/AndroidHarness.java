import fixture.AndroidBehavior;
import fixture.AndroidPeer;
import fixture.DeclarationOnly;
import fixture.fixture;
import java.lang.reflect.Modifier;

public class AndroidHarness {
    private static void emit(String key, long value) { System.out.println(key + "=" + value); }
    public static void main(String[] arguments) throws Exception {
        int[] scalars = {Integer.MIN_VALUE, Integer.MIN_VALUE + 1, -32769, -1, 0, 1, 32768, Integer.MAX_VALUE};
        AndroidPeer peer = new AndroidPeer();
        emit("namespace-identity", fixture.identity(peer) == peer ? 1 : 0);
        emit("namespace-null", fixture.identity(null) == null ? 1 : 0);
        for (int i = 0; i < scalars.length; i++) {
            int x = scalars[i];
            AndroidBehavior object = new AndroidBehavior(x);
            emit("instance-get:" + i, object.get());
            emit("instance-add:" + i, object.add(-37));
            emit("instance-exchange:" + i, object.exchange(scalars[scalars.length - i - 1]));
            emit("instance-after:" + i, object.get());
            emit("factory:" + i, AndroidBehavior.factory(x).get());
            emit("cross-class:" + i, AndroidBehavior.crossClass(x));
            emit("namespace-call:" + i, fixture.twice(x));
            for (int j = 0; j < scalars.length; j++) {
                emit("scalar:" + i + ":" + j, AndroidBehavior.scalar(x, scalars[j]));
                emit("choose:" + i + ":" + j, AndroidBehavior.choose(x, scalars[j]));
                emit("divide:" + i + ":" + j, AndroidBehavior.safeDivide(x, scalars[j]));
            }
        }
        long[] wides = {Long.MIN_VALUE, Long.MIN_VALUE + 1, -4294967296L, -1, 0, 4294967296L, Long.MAX_VALUE};
        for (int i = 0; i < wides.length; i++) {
            for (int j = 0; j < wides.length; j++) emit("wide:" + i + ":" + j, AndroidBehavior.wide(wides[i], wides[j]));
        }
        int[] floatBits = {0, 0x80000000, 0x7f800000, 0xff800000, 0x7fc12345, 1, 0xc0f00000};
        long[] doubleBits = {0L, 0x8000000000000000L, 0x7ff0000000000000L, 0xfff0000000000000L,
                             0x7ff8000000001234L, 1L, 0xc020800000000000L};
        for (int i = 0; i < floatBits.length; i++)
            emit("float-bits:" + i, Float.floatToRawIntBits(AndroidBehavior.floatIdentity(Float.intBitsToFloat(floatBits[i]))));
        for (int i = 0; i < doubleBits.length; i++)
            emit("double-bits:" + i, Double.doubleToRawLongBits(AndroidBehavior.doubleIdentity(Double.longBitsToDouble(doubleBits[i]))));
        for (int i = -3; i <= 3; i++) {
            emit("float-add:" + i, Float.floatToRawIntBits(AndroidBehavior.floatAdd(i * 2.25f, -7.5f)));
            emit("double-add:" + i, Double.doubleToRawLongBits(AndroidBehavior.doubleAdd(i * 3.125, -1024.5)));
            emit("mixed:" + i, Double.doubleToRawLongBits(AndroidBehavior.mixed(i, 10000000000L, 2.5f, -13.25)));
        }
        int[] values = new int[35];
        for (int i = 0; i < values.length; i++) values[i] = i % 7 == 0 ? Integer.MIN_VALUE : i % 7 == 1 ? Integer.MAX_VALUE : i * i - 97;
        for (int count = -1; count <= values.length; count++) emit("sum:" + count, AndroidBehavior.sum(values, count));
        for (int i = 0; i < 5; i++) {
            emit("array-swap:" + i, AndroidBehavior.exchangeArray(values, i, scalars[i]));
            emit("array-after:" + i, values[i]);
        }
        int[] array = AndroidBehavior.array();
        emit("array-length", array.length);
        for (int i = 0; i < array.length; i++) emit("array-data:" + i, array[i]);
        int[] branches = {Integer.MIN_VALUE, -10000, -1, 0, 1, 2, 3, 4, 100, 333, Integer.MAX_VALUE};
        for (int i = 0; i < branches.length; i++) {
            emit("packed:" + i, AndroidBehavior.packed(branches[i]));
            emit("sparse:" + i, AndroidBehavior.sparse(branches[i]));
        }
        emit("null-zero", AndroidBehavior.sum(null, 0));
        try { AndroidBehavior.sum(null, 1); emit("null-read", 0); }
        catch (NullPointerException expected) { emit("null-read", 1); }
        try { AndroidBehavior.sum(new int[0], 1); emit("array-bounds", 0); }
        catch (ArrayIndexOutOfBoundsException expected) { emit("array-bounds", 1); }
        emit("static-seed", AndroidBehavior.seed);
        emit("static-once", AndroidBehavior.initializationCount);
        emit("static-float-zero", Float.floatToRawIntBits(AndroidBehavior.class.getField("NEGATIVE_ZERO").getFloat(null)));
        emit("static-double-zero", Double.doubleToRawLongBits(AndroidBehavior.class.getField("DOUBLE_NEGATIVE_ZERO").getDouble(null)));
        emit("static-wide", AndroidBehavior.class.getField("WIDE_CONSTANT").getLong(null));
        String word = (String) AndroidBehavior.class.getField("WORD").get(null);
        emit("string-length", word.length());
        for (int i = 0; i < word.length(); i++) emit("string-unit:" + i, word.charAt(i));
        emit("abstract-declaration", Modifier.isAbstract(DeclarationOnly.class.getDeclaredMethod("abstractValue", int.class).getModifiers()) ? 1 : 0);
        emit("native-declaration", Modifier.isNative(DeclarationOnly.class.getDeclaredMethod("nativeValue", long.class).getModifiers()) ? 1 : 0);
        DeclarationOnly implementation = new DeclarationOnly() {
            public int abstractValue(int value) { return value + 7; }
        };
        emit("abstract-subclass", implementation.abstractValue(-31));
    }
}
