package fixture;
public class Parent extends Ancestor {
    public final long inputSnapshot, wideSnapshot;
    public final int codeSnapshot;
    public final Object tokenSnapshot;
    public Parent(long input, int code, long wide, Object token) {
        super();
        Trace.hit(5);
        this.inputSnapshot=input; this.codeSnapshot=code;
        Trace.hit(6);
        this.wideSnapshot=wide; this.tokenSnapshot=token;
        Trace.hit(7);
    }
}
