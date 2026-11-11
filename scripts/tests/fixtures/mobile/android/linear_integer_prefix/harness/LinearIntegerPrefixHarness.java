import fixture.*;
import java.lang.reflect.*;
public final class LinearIntegerPrefixHarness {
    static void require(boolean ok,String detail) { if(!ok)throw new AssertionError("linear integer prefix contract: "+detail); }
    public static void main(String[] args) throws Throwable {
        int flag=Integer.parseInt(args[0]),route=Integer.parseInt(args[1]),mode=Integer.parseInt(args[2]);
        boolean initialized=false,failed=false;
        for(int attempt=0;attempt<2;attempt++) {
            Trace.fail=mode==2?2:mode==3?1:mode>=4?mode-1:0;
            Trace.events=Trace.reads=Trace.calls=0;Trace.observed=-99;Trace.base=null;Trace.seen=null;
            Trace.failure=new IllegalStateException("owned prefix failure");
            Provider provider=mode==1?null:new Provider();boolean initial=(flag+attempt)%2!=0;
            if(provider!=null)provider.flag=initial;Trace.input=provider;
            int expected=0,reads=0,calls=0;boolean body=false,published=false,toggled=false,stored=false;
            Class<?> expectedError=null;
            if(failed)expectedError=NoClassDefFoundError.class;
            else if(!initialized) {
                initialized=true;expected=2;
                if(Trace.fail==2){failed=true;expectedError=ExceptionInInitializerError.class;}
            }
            if(expectedError==null) {
                if(provider==null)expectedError=NullPointerException.class;
                else {
                    expected=expected*10+1;reads=1;
                    if(Trace.fail==1){expectedError=IllegalStateException.class;body=true;}
                }
            }
            if(expectedError==null) {
                published=true;expected=expected*10+3;
                if(Trace.fail==3){expectedError=IllegalStateException.class;body=true;}
            }
            if(expectedError==null) {
                calls=1;expected=expected*10+4;
                if(Trace.fail==4){expectedError=IllegalStateException.class;body=true;}
                else toggled=true;
            }
            if(expectedError==null) {
                expected=expected*10+5;
                if(Trace.fail==5){expectedError=IllegalStateException.class;body=true;}
            }
            if(expectedError==null) {
                stored=true;expected=expected*10+6;
                if(Trace.fail==6){expectedError=IllegalStateException.class;body=true;}
            }
            Core result=null;Throwable error=null;
            try {
                if(route==0)result=LinearIntegerPrefixCaller.make(provider);
                else try { result=Core.class.getConstructor(Provider.class).newInstance(provider); }
                catch(InvocationTargetException failure){throw failure.getCause();}
            } catch(Throwable failure){error=failure;}
            require(Trace.events==expected,"original effect order");
            require(expectedError==null?error==null:error!=null&&error.getClass()==expectedError,"failure class");
            if(body)require(error==Trace.failure,"body failure identity");
            if(expectedError==ExceptionInInitializerError.class)require(error.getCause()==Trace.failure,"initializer cause");
            require(Trace.reads==reads&&Trace.calls==calls&&Trace.seen==(reads==0?null:provider),"getter and callback identity/count");
            require((Trace.base!=null)==published,"partial superclass publication");
            require(Trace.observed==(toggled?0:-99),"callback sees allocation default");
            if(provider!=null)require(provider.flag==(toggled?!initial:initial),"later parent mutation");
            if(published) {
                require(Trace.base instanceof Core,"original receiver class");Core partial=(Core)Trace.base;
                require(partial.argument==(initial?0:1),"boolean integer argument");
                require(partial.provider==(stored?provider:null)&&partial.stamp==(stored?17:0),"partial final fields");
            }
            if(expectedError==null)require(result==Trace.base,"result identity");
            System.out.println("flag="+flag+",route="+route+",mode="+mode+",attempt="+attempt+",trace="+Trace.events+",error="+(error==null?"none":error.getClass().getSimpleName()));
        }
    }
}
