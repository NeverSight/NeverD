package fixture;
public class Values {
    public Object object;
    public volatile float small;
    public volatile long wide;
    public double real;
    public boolean flag;
    public byte bite;
    public char code;
    public short half;
    public int word;
    public Object[] array;
    public void assign(Values value) {
        object=value.object;
        small=value.small;
        wide=value.wide;
        real=value.real;
        flag=value.flag;
        bite=value.bite;
        code=value.code;
        half=value.half;
        word=value.word;
        array=value.array;
    }
}
