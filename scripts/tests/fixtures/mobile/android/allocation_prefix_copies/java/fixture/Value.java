package fixture;
public final class Value {
    public final long bits;
    public final Object reference;
    public final int kind;
    public Value(long bits,Object reference,int kind) {
        int index=++Trace.allocations;
        if(index==1) Trace.first=this; else Trace.second=this;
        Trace.step(index*2-1);
        this.bits=bits;this.reference=reference;this.kind=kind;
        Trace.step(index*2);
    }
    public Value(boolean input) { this(input?1L:0L,null,0); }
    public Value(byte input) { this((long)input,null,1); }
    public Value(char input) { this((long)input,null,2); }
    public Value(short input) { this((long)input,null,3); }
    public Value(int input) { this((long)input,null,4); }
    public Value(long input) { this(input,null,5); }
    public Value(float input) { this((long)Float.floatToRawIntBits(input),null,6); }
    public Value(double input) { this(Double.doubleToRawLongBits(input),null,7); }
    public Value(Object input) { this(0L,input,8); }
    public Value(Object[] input) { this(0L,input,9); }
}
