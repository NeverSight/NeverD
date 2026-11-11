package fixture;
public final class Core {
    private static final int A=Trace.seed();
    private static final long B=Trace.wide();
    private static final float C=Trace.single();
    private static final double D=Trace.real();
    public static final int zzPad=37;
    public final int a;
    public final long b;
    public final float c;
    public final double d;
    public final Object token;
    public Core(int aa,long bb,float cc,double dd,Object value) {
        Trace.partial=this;Trace.step(6);
        this.a=aa;this.b=bb;this.c=cc;this.d=dd;this.token=value;
        Trace.step(7);
    }
    public Core(int aa,long bb,float cc,double dd,Object value,int mask) {
        this((mask&1)!=0?A:aa,(mask&2)!=0?B:bb,(mask&4)!=0?C:cc,(mask&8)!=0?D:dd,Trace.token(value));
    }
    public Core() {this(-1,-2L,-3.0f,-4.0d,null,15);}
}
