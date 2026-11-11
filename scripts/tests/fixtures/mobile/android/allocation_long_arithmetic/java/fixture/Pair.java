package fixture;
public final class Pair {
    public final long a,b,c,d,e,f;
    public Pair(long a,long b,long c,long d,long e,long f) {
        Trace.partial=this; Trace.hit(4);
        this.a=a;this.b=b;this.c=c; Trace.hit(5);
        this.d=d;this.e=e;this.f=f; Trace.hit(6);
    }
}
