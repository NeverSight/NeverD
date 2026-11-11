package fixture;
public class Parent {
 static {Trace.hit(1);}
 public final long id;public final Marker marker;
 public final Callback<Object,Token> first,second;
 public Parent(long id,Marker marker,Callback<Object,Token> first,Callback<Object,Token> second){
  Trace.parent=this;Trace.hit(4);this.id=id;this.marker=marker;this.first=first;this.second=second;Trace.hit(5);
 }
 public Token run(Object value){Trace.arg=value;Trace.hit(6);Token result=second.invoke(value);Trace.after=result;Trace.hit(8);return result;}
}
