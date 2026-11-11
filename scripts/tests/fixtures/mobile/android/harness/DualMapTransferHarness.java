import fixture.*;
import java.util.*;
import java.lang.reflect.*;
import java.lang.invoke.*;
public final class DualMapTransferHarness {
    static void require(boolean ok) { if(!ok)throw new AssertionError("owned dual map transfer"); }
    static Map call(int route,Map first,Map second) throws Throwable {
        try {
            if(route==0)return Client.call(first,second);
            if(route==1)return DualMapTransferCaller.call(first,second);
            if(route==2)return (Map)Client.class.getMethod("call",Map.class,Map.class).invoke(null,first,second);
            return (Map)MethodHandles.publicLookup().findStatic(Client.class,"call",MethodType.methodType(Map.class,Map.class,Map.class)).invokeExact(first,second);
        } catch(InvocationTargetException failure) { throw failure.getCause(); }
    }
    public static void main(String[] args) throws Throwable {
        int route=Integer.parseInt(args[0]),mode=Integer.parseInt(args[1]);
        Method method=Provider.class.getMethod("merge",Map.class,Map.class);TypeVariable<?>[] formals=method.getTypeParameters();
        require(formals.length==2 && formals[0].getBounds()[0]==Object.class && formals[1].getBounds()[0]==Object.class);
        for(Type parameter:method.getGenericParameterTypes()) {
            Type[] slots=((ParameterizedType)parameter).getActualTypeArguments();
            for(int i=0;i<2;i++)require(((WildcardType)slots[i]).getUpperBounds()[0].equals(formals[i]));
        }
        require(Arrays.equals(((ParameterizedType)method.getGenericReturnType()).getActualTypeArguments(),formals));
        Trace.failure=new RuntimeException("owned map failure");Trace.failAt=mode>=1&&mode<=7?mode:0;
        Trace.kind=mode==10?1:mode==11?2:mode==12?3:0;
        Object key=mode==14?new Object():new String("owned key"),equalKey=mode==14?key:new String("owned key");
        Object left=new String("left"),right=mode==13?new Object():new String("right"),extra=new Object(),otherValue=new Object();
        Map firstMap=new LinkedHashMap(),secondMap=new LinkedHashMap(),other=new LinkedHashMap();
        firstMap.put(key,left);secondMap.put(equalKey,right);secondMap.put("extra",extra);other.put(key,otherValue);
        Map first=mode==8?null:firstMap,second=mode==9?null:mode==15?firstMap:secondMap;
        Trace.other=other;Object sentinel=new Object();
        for(int attempt=0;attempt<2;attempt++) {
            Trace.events=0;Trace.first=Trace.second=Trace.created=Trace.after=sentinel;
            Throwable failure=null;Map actual=null;
            try { actual=call(route,first,second); } catch(Throwable error) { failure=error; }
            long expected=mode>=1&&mode<=7?Long.parseLong("1234567".substring(0,mode)):mode==8?1234:mode==9?12345:1234567;
            require(Trace.events==expected);
            require(Trace.first==(mode==1?sentinel:first)&&Trace.second==(mode==1||mode==2?sentinel:second));
            boolean created=mode==0||mode>=4;
            require((Trace.created!=sentinel)==created);
            if(created) {
                Map copied=(Map)Trace.created;
                if(mode==4||mode==8)require(copied.isEmpty());
                else if(mode==5||mode==9||mode==15)require(copied.size()==1&&copied.get(key)==left&&copied.keySet().iterator().next()==key);
                else require(copied.size()==2&&copied.get(key)==right&&copied.get("extra")==extra&&copied.keySet().iterator().next()==key);
                require(copied!=firstMap&&copied!=secondMap);
            }
            require(firstMap.size()==1&&firstMap.get(key)==left&&secondMap.size()==2&&secondMap.get(equalKey)==right);
            if(mode>=1&&mode<=7)require(failure==Trace.failure);
            else if(mode==8||mode==9)require(failure instanceof NullPointerException);
            else require(failure==null&&actual==(mode==10?other:mode==11?null:mode==12?firstMap:Trace.created));
            boolean reached=mode==0||mode==7||mode>=10;
            require(Trace.after==(reached?(mode==10?other:mode==11?null:mode==12?firstMap:Trace.created):sentinel));
            if(failure==null&&actual==Trace.created) {actual.put("mutable",otherValue);require(actual.get("mutable")==otherValue&&!firstMap.containsKey("mutable")&&!secondMap.containsKey("mutable"));}
            System.out.println("route="+route+",mode="+mode+",attempt="+attempt+",trace="+Trace.events);
        }
    }
}
