package fixture;
public class Target<T> extends java.util.LinkedHashMap<String,Object> {
    public T value;
    public java.util.List<Target<T>> values;
    public Target(T value){this.value=value;}
    public java.util.List<Target<T>> view(){Trace.views++;Trace.viewer=this;Trace.hit(4);return values;}
}
