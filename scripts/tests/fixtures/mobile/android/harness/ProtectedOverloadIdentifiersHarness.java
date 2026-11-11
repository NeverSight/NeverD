import api.*;
import impl.*;
import java.lang.reflect.*;
public class ProtectedOverloadIdentifiersHarness {
    static void require(boolean value){if(!value)throw new AssertionError("owned protected overload identifiers");}
    public static void main(String[]args)throws Throwable {
        int route=Integer.parseInt(args[0]),kind=Integer.parseInt(args[1]),shape=Integer.parseInt(args[2]),mode=Integer.parseInt(args[3]);String name=args[4];
        Method root=Root.class.getDeclaredMethod(name,long.class,float.class,Callback.class);
        Method tagged=Root.class.getDeclaredMethod(name,long.class,float.class,Token.class);
        require(Modifier.isProtected(root.getModifiers())&&Modifier.isAbstract(root.getModifiers())&&Modifier.isProtected(tagged.getModifiers())&&!Modifier.isAbstract(tagged.getModifiers()));
        for(Class c:new Class[]{Root.class,Left.class,Right.class}) {
            Method m=c.getDeclaredMethod(name,long.class,float.class,Callback.class);Type t=m.getGenericParameterTypes()[2];
            require(t instanceof ParameterizedType&&m.getTypeParameters().length==0);
            Type[] p=((ParameterizedType)t).getActualTypeArguments();require(p.length==2&&p[0] instanceof WildcardType&&((WildcardType)p[0]).getLowerBounds()[0]==Context.class&&p[1]==Object.class);
        }
        long[] longs={Long.MIN_VALUE,Long.MAX_VALUE,0L,-1L,0x1234567887654321L,0xfedcba9876543210L,0x100000000L,0xffffffffL};
        int[] floats={0x7fc01234,0xffc05678,0,0x80000000,0x7f800000,0xff800000,1,0x7f7fffff};
        for(int attempt=0;attempt<2;attempt++) {
            Trace.events=0;Trace.bits=0;Trace.raw=0;Trace.kind=0;Trace.ownerSeen=null;Trace.argSeen=null;Trace.callbackInput=null;
            Trace.fail=mode>=4&&mode<18?(mode-4)%7+1:0;Trace.failure=mode>=11&&mode<18?new AssertionError("owned error"):new IllegalStateException("owned runtime");
            Root owner=kind==0?new Left():new Right();ProtectedOverloadIdentifiersProbe probe=attempt==0?new ProtectedOverloadIdentifiersProbe():new ProtectedOverloadIdentifiersProbe.Child();
            Object arg=mode==19?null:probe;Context context=mode%3==0?null:probe;Trace.context=context;
            Object answer=mode%4==0?new Object():mode%4==1?null:mode%4==2?owner:arg;Trace.result=answer;
            long bits=longs[(mode+attempt)%8];int raw=floats[(mode+attempt)%8];float scale=Float.intBitsToFloat(raw);
            Throwable failure=null;try{ProtectedOverloadIdentifiersCaller.call(route,shape,owner,bits,scale,arg,name);}catch(Throwable e){failure=e;}
            int[] steps=shape==0?(kind==0?new int[]{1,2,3,4,5,6}:new int[]{1,7,2,3,4,5,6}):(kind==0?new int[]{1,2,3,5,6}:new int[]{1,7,2,3,5,6});
            long events=0;boolean captured=false,called=false;Throwable wanted=null;
            for(int step:steps) {
                if(step==4&&arg==null){wanted=failure;require(failure instanceof NullPointerException);break;}
                if(step==2)captured=true;if(step==4)called=true;
                events=events*10+step;if(step==Trace.fail){wanted=Trace.failure;break;}
            }
            require(failure==wanted&&Trace.events==events);
            require(Trace.ownerSeen==(captured?owner:null)&&Trace.argSeen==(captured?arg:null)&&Trace.bits==(captured?bits:0)&&Trace.raw==(captured?raw:0));
            require(Trace.kind==(captured?(shape==1?1:kind==0?2:3):0)&&Trace.callbackInput==(called?context:null));
            require(Trace.context==context&&Trace.result==answer);
            System.out.println("route="+route+",kind="+kind+",shape="+shape+",mode="+mode+",attempt="+attempt+",events="+Trace.events);
        }
    }
}
