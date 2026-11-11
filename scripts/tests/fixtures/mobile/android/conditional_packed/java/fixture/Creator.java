package fixture;
public final class Creator {
    public Target make(Object left,Object right,float first,float second,int shift,long mask,int low,int high) {
        return new Target(Provider.read(left),left==right
                ? (((long)Float.floatToRawIntBits(first)<<shift) | ((long)Float.floatToRawIntBits(second)&mask))
                : (((long)Float.floatToRawIntBits(second)<<shift) ^ ((long)Float.floatToRawIntBits(first)&mask)),
                Provider.after());
    }
}
