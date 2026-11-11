package fixture;public final class Quad {
 static {Trace.initializations++;Trace.hit(9);}
 public final float a,b,c,d;
 public Quad(float a,float b,float c,float d){Trace.partial=this;Trace.hit(2);this.a=a;this.b=b;this.c=c;this.d=d;Trace.hit(3);}
}
