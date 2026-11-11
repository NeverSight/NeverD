package fixture;
public final class Core {
    public final String name;
    public final Object context;
    public final float add,sub,mul,div,rem;
    public final double wideAdd,wideSub,wideMul,wideDiv,wideRem;
    public Core(String name,float fa,float fs,float fm,float fd,float fr,double da,double ds,double dm,double dd,double dr,Object context) {
        Trace.partial=this;Trace.step(2);
        this.add=fa;Trace.step(3);
        this.sub=fs;this.mul=fm;this.div=fd;this.rem=fr;
        this.wideAdd=da;this.wideSub=ds;this.wideMul=dm;this.wideDiv=dd;this.wideRem=dr;
        this.name=name;this.context=context;Trace.step(4);
    }
    public Core(float a,float b,double c,double d,Object context) {
        this(Trace.nameValue(),a+b,a-b,a*b,a/b,a%b,c+d,c-d,c*d,c/d,c%d,context);
    }
    public Core(float a,float b,float e,double c,double d,Object context) {
        this((a+b)+e,(b-e)*a,c/d,c%d,context);
    }
    public Core(float a,float b,float e,int marker) { this(a,b,e,(a+b)+e); }
    public Core(float a,float b,float e,float sum) { this(a/sum,b/sum,1.0,2.0,null); }

}
