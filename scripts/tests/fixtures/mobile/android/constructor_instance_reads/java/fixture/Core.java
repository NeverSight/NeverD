package fixture;
public final class Core {
    private Object rawObject;
    private volatile float rawFloat;
    private volatile long rawWide;
    private double rawDouble;
    private boolean rawFlag;
    private byte rawByte;
    private char rawChar;
    private short rawShort;
    private int rawInt;
    private Object[] rawArray;
    public final String name;
    public final Object object;
    public final float small;
    public final long wide;
    public final double real;
    public final boolean flag;
    public final byte bite;
    public final char code;
    public final short half;
    public final int word;
    public final Object[] array;
    public final Object[] replacement;
    public final Object context;
    public Core(String name, Object object, float small, long wide, double real, boolean flag, byte bite, char code, short half, int word, Object[] array, Object[] replacement, Object context) {
        Trace.partial=this;
        Trace.step(3);
        this.name=name;
        this.object=object;
        Trace.step(4);
        this.small=small;
        this.wide=wide;
        this.real=real;
        this.flag=flag;
        this.bite=bite;
        this.code=code;
        this.half=half;
        this.word=word;
        this.array=array;
        this.replacement=replacement;
        this.context=context;
        Trace.step(5);
    }
    public Core(Core seed, Object[] replacement, Object context) {
        this(seed.getName(), seed.rawObject, seed.rawFloat, seed.rawWide, seed.rawDouble, seed.rawFlag, seed.rawByte, seed.rawChar, seed.rawShort, seed.rawInt, seed.rawArray, replacement, context);
    }
    public Core(Values value, Object[] replacement, Object context) {
        this(Trace.name, value.object, value.small, value.wide, value.real, value.flag, value.bite, value.code, value.half, value.word, value.array, replacement, context);
    }
    public String getName() {
        String snapshot=Trace.name;
        Trace.getter=this;
        Trace.step(1);
        set(Trace.afterGetter);
        Trace.step(2);
        return snapshot;
    }
    public void set(Values value) {
        rawObject=value.object;
        rawFloat=value.small;
        rawWide=value.wide;
        rawDouble=value.real;
        rawFlag=value.flag;
        rawByte=value.bite;
        rawChar=value.code;
        rawShort=value.half;
        rawInt=value.word;
        rawArray=value.array;
    }
}
