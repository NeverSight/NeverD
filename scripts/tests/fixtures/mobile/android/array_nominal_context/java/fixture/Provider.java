package fixture;
public class Provider {
    public static <T> String join(T[] values,CharSequence delimiter){return full(values,delimiter,null,null);}
    public static <T> String full(T[] values,CharSequence delimiter,String prefix,String suffix){
        Trace.hit(1);Trace.values=values;Trace.delimiter=delimiter;Trace.prefix=prefix;Trace.suffix=suffix;Trace.action.run();
        Trace.hit(2);if(values!=null&&values.length!=0){Object value=values[0];Trace.element=value;Trace.text=value==null?null:value.toString();}
        Trace.hit(3);if(delimiter!=null)Trace.delimiterText=delimiter.toString();Trace.hit(4);return Trace.result;
    }
}
