package fixture;

public final class Token implements Cloneable {
    public final int id;
    public Token(int id){this.id=id;}
    @Override public Object clone(){return Trace.clone(this);}
}
