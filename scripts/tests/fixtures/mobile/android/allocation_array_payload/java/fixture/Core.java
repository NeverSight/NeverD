package fixture;
public class Core {
    public static Target create(Object input) { return new Target(Trace.before(input),
                new boolean[]{false, true, false, true, true, false, true},
                new byte[]{-128, 127, -1, 0, 17, -23, 63},
                new char[]{'\u0000', '\uffff', 'x', '\u1234', 'y', 'z', 'a'},
                new short[]{-32768, 32767, -1, 0, 17, -23, 63},
                new int[]{Integer.MIN_VALUE, Integer.MAX_VALUE, -1, 0, 17, -23, 63},
                new long[]{Long.MIN_VALUE, Long.MAX_VALUE, -1L, 0L, 17L, -23L, 63L},
                new float[]{-0.0f, Float.NaN, Float.POSITIVE_INFINITY, Float.NEGATIVE_INFINITY, Float.MIN_VALUE, Float.MAX_VALUE, -3.25f},
                new double[]{-0.0, Double.NaN, Double.POSITIVE_INFINITY, Double.NEGATIVE_INFINITY, Double.MIN_VALUE, Double.MAX_VALUE, -3.25}, Trace.after(input)); }
    public static Target guarded(Object input) {
        try { return new Target(Trace.before(input),
                new boolean[]{false, true, false, true, true, false, true},
                new byte[]{-128, 127, -1, 0, 17, -23, 63},
                new char[]{'\u0000', '\uffff', 'x', '\u1234', 'y', 'z', 'a'},
                new short[]{-32768, 32767, -1, 0, 17, -23, 63},
                new int[]{Integer.MIN_VALUE, Integer.MAX_VALUE, -1, 0, 17, -23, 63},
                new long[]{Long.MIN_VALUE, Long.MAX_VALUE, -1L, 0L, 17L, -23L, 63L},
                new float[]{-0.0f, Float.NaN, Float.POSITIVE_INFINITY, Float.NEGATIVE_INFINITY, Float.MIN_VALUE, Float.MAX_VALUE, -3.25f},
                new double[]{-0.0, Double.NaN, Double.POSITIVE_INFINITY, Double.NEGATIVE_INFINITY, Double.MIN_VALUE, Double.MAX_VALUE, -3.25}, Trace.after(input)); } catch (Throwable failure) { Trace.last = failure; return null; }
    }
    public static Target locked(Object input) {
        synchronized (Trace.lock) { return new Target(Trace.before(input),
                new boolean[]{false, true, false, true, true, false, true},
                new byte[]{-128, 127, -1, 0, 17, -23, 63},
                new char[]{'\u0000', '\uffff', 'x', '\u1234', 'y', 'z', 'a'},
                new short[]{-32768, 32767, -1, 0, 17, -23, 63},
                new int[]{Integer.MIN_VALUE, Integer.MAX_VALUE, -1, 0, 17, -23, 63},
                new long[]{Long.MIN_VALUE, Long.MAX_VALUE, -1L, 0L, 17L, -23L, 63L},
                new float[]{-0.0f, Float.NaN, Float.POSITIVE_INFINITY, Float.NEGATIVE_INFINITY, Float.MIN_VALUE, Float.MAX_VALUE, -3.25f},
                new double[]{-0.0, Double.NaN, Double.POSITIVE_INFINITY, Double.NEGATIVE_INFINITY, Double.MIN_VALUE, Double.MAX_VALUE, -3.25}, Trace.after(input)); }
    }
}
