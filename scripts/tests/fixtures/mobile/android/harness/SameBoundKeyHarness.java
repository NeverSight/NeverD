import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
public final class SameBoundKeyHarness {
    static int reads;
    static Object token;
    static Key seen;
    static RuntimeException failure;
    static void require(boolean ok){if(!ok)throw new AssertionError("owned same bound key");}
    static final class Value implements Element {
        final long v;Value(long v){this.v=v;}public long value(){return v;}
    }
    static final class Read implements Key {
        final Element value;final boolean failing;
        Read(Element value,boolean failing){this.value=value;this.failing=failing;}
        public Element read(){reads++;seen=this;if(failing)throw failure;return value;}
    }
    static Element call(int route,Provider provider,Key key) throws Throwable {
        if(route==0)return Client.call(provider,key);
        if(route==1)return SameBoundKeyCaller.call(provider,key);
        if(route==2){try{return (Element)Provider.class.getMethod("get",Key.class).invoke(provider,key);}catch(InvocationTargetException p){throw p.getCause();}}
        return (Element)MethodHandles.publicLookup().findVirtual(Provider.class,"get",MethodType.methodType(Element.class,Key.class)).invokeExact(provider,key);
    }
    public static void main(String[] args) throws Throwable {
        int route=Integer.parseInt(args[0]),mode=Integer.parseInt(args[1]);
        Method method=Provider.class.getDeclaredMethod("get",Key.class);
        require(method.getTypeParameters().length==1 && method.getTypeParameters()[0].getBounds()[0]==Element.class && method.getGenericReturnType().equals(method.getTypeParameters()[0]));
        ParameterizedType input=(ParameterizedType)method.getGenericParameterTypes()[0];
        require(input.getRawType()==Key.class && input.getActualTypeArguments()[0].equals(method.getTypeParameters()[0]));
        Provider provider=mode==2?null:new Provider();
        if(provider!=null)provider.skip=mode==6||mode==7;
        for(int attempt=0;attempt<2;attempt++){
            Element value=mode==1?null:new Value(mode==8?Long.MIN_VALUE:mode==9?Long.MAX_VALUE:0x1234567887654321L+attempt);
            Key key=mode==3||mode==7?null:new Read(value,mode==4);
            if(mode==5)key=(Key)Proxy.newProxyInstance(Key.class.getClassLoader(),new Class<?>[]{Key.class},(o,m,a)->{reads++;seen=(Key)o;return new Object();});
            reads=0;seen=null;failure=new RuntimeException("owned key failure");
            if(provider!=null){provider.seen=null;provider.returned=null;}
            Element result=null;Throwable caught=null;
            try{result=call(route,provider,key);}catch(Throwable p){caught=p;}
            boolean skip=mode==6||mode==7;
            require(reads==((mode==2||mode==3||skip)?0:1));
            if(mode==2||mode==3)require(caught instanceof NullPointerException);
            else if(mode==4)require(caught==failure);
            else if(mode==5)require(caught instanceof ClassCastException);
            else require(caught==null && result==(skip?null:value));
            if(provider!=null){require(provider.visits==attempt+1 && provider.seen==key);require(provider.returned==((mode==3||mode==4||mode==5||skip)?null:value));}
            if(reads!=0)require(seen==key);
            if(result!=null)require(result.value()==value.value());
            System.out.println("route="+route+",mode="+mode+",attempt="+attempt+",trace="+reads);
        }
    }
}
