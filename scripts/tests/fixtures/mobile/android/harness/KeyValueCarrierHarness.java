import fixture.*;
import java.lang.reflect.*;
import java.util.*;
import kotlin.jvm.functions.Function1;
public final class KeyValueCarrierHarness {
    static String events;
    static Object key,stored;
    static boolean present;
    static int mode;
    static RuntimeException failure;
    static void require(boolean value){if(!value)throw new AssertionError("owned key value carrier");}
    static final class Key {
        public String toString(){events+="s";if(mode==19)throw failure;return "owned-key";}
        public int hashCode(){throw new AssertionError("new key hash effect");}
        public boolean equals(Object other){throw new AssertionError("new key equality effect");}
    }
    static final class Numeric extends Number {
        public int intValue(){events+="n";if(mode==18)throw failure;return 29;}
        public long longValue(){throw new AssertionError("changed number conversion");}
        public float floatValue(){throw new AssertionError("changed number conversion");}
        public double doubleValue(){throw new AssertionError("changed number conversion");}
    }
    static final class Tracked extends AbstractMap<Object,Object> {
        public Set<Entry<Object,Object>> entrySet(){throw new AssertionError("new map traversal");}
        public Object get(Object given){require(given==key);events+="g";if(mode==7)throw failure;return stored;}
        public boolean containsKey(Object given){require(given==key);events+="c";if(mode==8)throw failure;return present;}
    }
    static void metadata()throws Exception{
        for(String name:new String[]{"lookup","direct"}){
            Method method=CoreKt.class.getDeclaredMethod(name,Map.class,Object.class);TypeVariable<?>[] own=method.getTypeParameters();
            require(own.length==2 && own[0].getGenericDeclaration().equals(method) && own[1].getGenericDeclaration().equals(method));
            require(own[0].getBounds()[0]==Object.class && own[1].getBounds()[0]==Object.class);
            ParameterizedType map=(ParameterizedType)method.getGenericParameterTypes()[0];require(map.getRawType()==Map.class);
            require(map.getActualTypeArguments()[0].equals(own[0]) && ((WildcardType)map.getActualTypeArguments()[1]).getUpperBounds()[0].equals(own[1]));
            require(method.getGenericParameterTypes()[1].equals(own[0]) && method.getGenericReturnType().equals(own[1]));
        }
    }
    @SuppressWarnings({"rawtypes","unchecked"})
    static Object call(int route,Map map,Object key){
        switch(route){case 0:return CoreKt.lookup(map,key);case 1:return CoreKt.read(map,key);case 2:return CoreKt.direct(map,key);default:return CoreKt.directRead(map,key);}
    }
    public static void main(String[] args)throws Exception{
        int route=Integer.parseInt(args[0]);mode=Integer.parseInt(args[1]);metadata();Throwable previous=null;
        for(int attempt=0;attempt<2;attempt++){
            events="";key=new Key();stored=new Object();present=true;failure=new IllegalArgumentException("owned exact failure");
            Map<Object,Object> map=new Tracked();boolean defaults=mode==5 || mode==6 || mode==9 || mode==20 || mode==21;
            if(mode==1)stored=key;if(mode==2)stored=map;
            if(mode==3 || mode==4 || mode==5 || mode==6 || mode==8 || mode==9 || mode==10 || mode==18 || mode==19 || mode==20 || mode==21)stored=null;
            if(mode==4 || mode==5 || mode==6 || mode==9 || mode==10 || mode==19 || mode==20 || mode==21)present=false;
            if(mode==10 || mode==11 || mode==21)key=null;
            if(mode==12)map=null;if(mode==13)stored=Long.valueOf(37);if(mode==14)stored=Double.valueOf(7.75);if(mode==15)stored=Float.valueOf(-8.25f);if(mode==16)stored="different value type";
            if(mode==17 || mode==18)stored=new Numeric();
            final Object fallback=mode==6?null:mode==20 || mode==21?key:new Object();
            if(defaults)map=kotlin.collections.MapsKt.withDefault(map,new Function1<Object,Object>(){public Object invoke(Object given){require(given==key);events+="d";if(mode==9)throw failure;return fallback;}});
            Object expected=defaults?fallback:stored;Object result=null;Throwable seen=null;try{result=call(route,map,key);}catch(Throwable error){seen=error;}
            boolean numeric=(route&1)==1;String wanted=mode==12?"":"g";
            if(mode!=12 && mode!=7 && stored==null)wanted+="c";
            if(defaults && stored==null && !present)wanted+="d";
            boolean missing=mode==4 || mode==10 || mode==19;if(missing && key!=null)wanted+="s";
            if(numeric && (mode==17 || mode==18))wanted+="n";
            require(events.equals(wanted));
            if(mode==7 || mode==8 || mode==9 || mode==18 && numeric || mode==19)require(seen==failure);
            else if(missing){require(seen!=null && seen.getClass()==NoSuchElementException.class && seen!=previous);require(seen.getMessage().equals("Key "+(key==null?"null":"owned-key")+" is missing in the map."));}
            else if(mode==12 || numeric && expected==null)require(seen!=null && seen.getClass()==NullPointerException.class && seen!=previous);
            else if(numeric && !(expected instanceof Number))require(seen!=null && seen.getClass()==ClassCastException.class && seen!=previous);
            else if(numeric){int value=mode==17?29:mode==13?37:mode==14?7:mode==15?-8:0;require(seen==null && result instanceof Integer && ((Integer)result).intValue()==value);}
            else require(seen==null && result==expected);
            previous=seen;
        }
        System.out.println("attempts=2,key-value-carrier=matched");
    }
}
