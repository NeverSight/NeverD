package fixture;
public class Base {
    public Base source;
    public int code;
    public Base(Base input,int selected) {
        Trace.hit(2,this,input,selected);
        this.source=input;
        this.code=selected;
        Trace.hit(3,this,input,selected);
    }
}
