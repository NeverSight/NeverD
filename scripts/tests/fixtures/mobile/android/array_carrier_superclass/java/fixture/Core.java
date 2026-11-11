package fixture;
public final class Core extends Parent<Object,Object> {
    public int calls;
    public byte[] observed;
    public int[] marker;
    public RuntimeException failure;
    public Result result;
    public Core() {}
    public void write(Token token,Object value,int index,long wide) { writeValue(token,((Carrier)value).unbox(),index,wide); }
    public int count(Object value) { return countValue(((Carrier)value).unbox()); }
    public Object read(Object value) { return readValue(((Carrier)value).unbox()); }
    private void observe(byte[] value) {
        calls++; observed=value; if (marker != null) marker[0]+=5;
        if (failure != null) throw failure;
    }
    protected void writeValue(Token token,byte[] value,int index,long wide) {
        observe(value); token.receiver=this; token.accepted=value; token.index=index; token.wide=wide;
    }
    protected int countValue(byte[] value) { observe(value); return value == null ? -1 : value.length; }
    protected Result readValue(byte[] value) { observe(value); return result; }
}
