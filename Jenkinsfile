pipeline {
    agent any

    stages {
    //    parallel {
    //        stage('Build Linux app') {
    //            agent {
    //                docker {
    //                    image 'terminal:latest'
    //                    reuseNode true
    //                    args '-v /var/cache/3rd:/home/3rd'
    //                }
    //            }
    //            steps {
    //                sh "cd ./terminal && pip install requests"
    //                sh "cd ./terminal && python generate.py release"
    //        //        sh "cd ./terminal/terminal.release && make -j 16"
    //        //        sh "cd ./terminal/Deploy && ./deploy.sh"
    //            }
    //        }
            stage('Build MacOSX app') {
                steps {
                    sh "ssh Admin@10.1.60.206 pwd"
                    sh "scp ${WORKSPACE} Admin@10.1.60.206:~/Workspace"
            //        sh "cd ./terminal && pip install requests"
            //        sh "cd ./terminal && python generate.py release"
            //        sh "cd ./terminal/terminal.release && make -j 16"
            //        sh "cd ./terminal/Deploy && ./deploy.sh"
                }
     //       }
        }
        
     //   stage('Transfer') {
     //       steps {
     //           sh "scp ${WORKSPACE}/terminal/Deploy/bsterminal.deb genoa@10.0.1.36:/var/www/terminal/Linux/bsterminal_${TAG}.deb"
     //           sh "ssh genoa@10.0.1.36 ln -sf /var/www/terminal/Linux/bsterminal_${TAG}.deb /var/www/downloads/bsterminal.deb"
     //       }
     //   }
    }
}
