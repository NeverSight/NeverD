package fixture;
public class Base<T> {
    public final T first,second;public final Callback<T> callback;public final boolean left,right;public final T value;
    public Base(T first,T second,Callback<T> callback,boolean left,boolean right){this.first=first;this.second=second;this.callback=callback;this.left=left;this.right=right;Trace.receiver=this;Trace.hit(4);Trace.baseAction.run();this.value=first;Trace.hit(5);}
}
