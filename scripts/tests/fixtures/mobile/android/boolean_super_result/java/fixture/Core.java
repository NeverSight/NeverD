package fixture;
public class Core extends Base<String,Object> implements Tagged<String> {
    private String normalize(String value){Trace.hit(1);Trace.raw=value;Trace.normalizeAction.run();return Trace.normalized;}
    public boolean submit(String value,Object expected,Object update){String argument=normalize(value);boolean result=super.change(argument,expected,update);Trace.hit(4);return result;}
    public boolean checked(Object value,Object expected,Object update){String argument=normalize((String)value);boolean result=super.change(argument,expected,update);Trace.hit(4);return result;}
}
