package fixture; public class Factory {
 private static <S,T,V extends Vector> void consume(Outer<S> outer,Outer<S>.Member<T,V> member,T first,T second,Spec<T> spec,String label,int mode){
  Trace.outer=outer;Trace.member=member;Trace.first=first;Trace.second=second;Trace.label=label;Trace.mode=mode;Trace.hit(1);
  Trace.value=member.value();Trace.hit(2);Trace.vector=member.vector();Trace.hit(3);Trace.seed=spec.read();Trace.hit(4);
 }
 public static void access(Outer outer,Outer.Member member,Object first,Object second,Spec spec,String label,int mode){consume(outer,member,first,second,spec,label,mode);}
}
