package fixture;public final class Core {
 public static Quad make(float input){
  Object saved;
  Quad result=new Quad(switch(0){default->{saved=Trace.box(input);yield input;}},input,input,input);
  Trace.saved=saved;Trace.after=result;Trace.hit(4);return result;
 }
}
